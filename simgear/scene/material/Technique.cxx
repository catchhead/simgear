#include "Technique.hxx"
#include "Pass.hxx"

#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#include <iterator>
#include <vector>
#include <string>

#include <osg/GLExtensions>
#include <osg/Math>
#include <osgUtil/CullVisitor>

#include <osgDB/Registry>
#include <osgDB/Input>
#include <osgDB/ParameterOutput>

#include <simgear/props/props.hxx>
#include <simgear/structure/OSGUtils.hxx>

namespace simgear
{
using namespace osg;
using namespace osgUtil;

namespace
{

struct ValidateOperation : GraphicsOperation
{
    ValidateOperation(Technique* technique_)
        : GraphicsOperation(opName, false), technique(technique_)
    {
    }
    virtual void operator() (GraphicsContext* gc);
    osg::ref_ptr<Technique> technique;
    static const std::string opName;
};

const std::string ValidateOperation::opName("ValidateOperation");


void ValidateOperation::operator() (GraphicsContext* gc)
{
    technique->validateInContext(gc);
}
}

Technique::Technique(bool alwaysValid)
    : _alwaysValid(alwaysValid), _contextIdLocation(-1)
{
}

Technique::Technique(const Technique& rhs, const osg::CopyOp& copyop) :
    _contextMap(rhs._contextMap), _alwaysValid(rhs._alwaysValid),
    _shadowingStateSet(rhs._shadowingStateSet),
    _validExpression(rhs._validExpression),
    _contextIdLocation(rhs._contextIdLocation)
{
    using namespace std;
    using namespace boost;
    transform(rhs.passes.begin(), rhs.passes.end(),
              back_inserter(passes),
              bind(simgear::clone_ref<Pass>, _1, copyop));

}

Technique::~Technique()
{
}

Technique::Status Technique::valid(osg::RenderInfo* renderInfo)
{
    if (_alwaysValid)
        return VALID;
    unsigned contextID = renderInfo->getContextID();
    ContextInfo& contextInfo = _contextMap[contextID];
    Status status = contextInfo.valid();
    if (status != UNKNOWN)
        return status;
    Status newStatus = QUERY_IN_PROGRESS;
    // lock and spawn validity check.
    if (!contextInfo.valid.compareAndSwap(status, newStatus)) {
        // Lost the race with another thread spawning a request
        return contextInfo.valid();
    }
    ref_ptr<ValidateOperation> validOp = new ValidateOperation(this);
    GraphicsContext* context = renderInfo->getState()->getGraphicsContext();
    GraphicsThread* thread = context->getGraphicsThread();
    if (thread)
        thread->add(validOp.get());
    else
        context->add(validOp.get());
    return newStatus;
}

Technique::Status Technique::getValidStatus(const RenderInfo* renderInfo) const
{
    if (_alwaysValid)
        return VALID;
    ContextInfo& contextInfo = _contextMap[renderInfo->getContextID()];
    return contextInfo.valid();
}

void Technique::validateInContext(GraphicsContext* gc)
{
    unsigned int contextId = gc->getState()->getContextID();
    ContextInfo& contextInfo = _contextMap[contextId];
    Status oldVal = contextInfo.valid();
    Status newVal = INVALID;
    expression::FixedLengthBinding<1> binding;
    binding.getBindings()[_contextIdLocation].val.intVal = contextId;
    if (_validExpression->getValue(&binding))
        newVal = VALID;
    contextInfo.valid.compareAndSwap(oldVal, newVal);
}

namespace
{
enum NumDrawables {NUM_DRAWABLES = 128};
}

EffectGeode::DrawablesIterator
Technique::processDrawables(const EffectGeode::DrawablesIterator& begin,
                            const EffectGeode::DrawablesIterator& end,
                            CullVisitor* cv,
                            bool isCullingActive)
{
    RefMatrix& matrix = *cv->getModelViewMatrix();
    float depth[NUM_DRAWABLES];
    EffectGeode::DrawablesIterator itr = begin;
    bool computeNearFar
        = cv->getComputeNearFarMode() != CullVisitor::DO_NOT_COMPUTE_NEAR_FAR;
    for (int i = 0; i < NUM_DRAWABLES && itr != end; ++itr, ++i) {
        Drawable* drawable = itr->get();
        const BoundingBox& bb = drawable->getBound();
        if ((drawable->getCullCallback()
             && drawable->getCullCallback()->cull(cv, drawable,
                                                  &cv->getRenderInfo()))
            || (isCullingActive && cv->isCulled(bb))) {
            depth[i] = FLT_MAX;
            continue;
        }
        if (computeNearFar && bb.valid()) {
            if (!cv->updateCalculatedNearFar(matrix, *drawable, false)) {
                depth[i] = FLT_MAX;
                continue;
            }
        }
        depth[i] = (bb.valid()
                    ? cv->getDistanceFromEyePoint(bb.center(), false)
                    : 0.0f);
        if (isNaN(depth[i]))
            depth[i] = FLT_MAX;
    }
    EffectGeode::DrawablesIterator drawablesEnd = itr;
    BOOST_FOREACH(ref_ptr<Pass>& pass, passes)
    {
        cv->pushStateSet(pass.get());
        int i = 0;
        for (itr = begin; itr != drawablesEnd; ++itr, ++i) {
            if (depth[i] != FLT_MAX)
                cv->addDrawableAndDepth(itr->get(), &matrix, depth[i]);
        }
        cv->popStateSet();
    }
    return drawablesEnd;
}

void Technique::resizeGLObjectBuffers(unsigned int maxSize)
{
    if (_shadowingStateSet.valid())
        _shadowingStateSet->resizeGLObjectBuffers(maxSize);
    BOOST_FOREACH(ref_ptr<Pass>& pass, passes) {
        pass->resizeGLObjectBuffers(maxSize);
    }
    _contextMap.resize(maxSize);
}

void Technique::releaseGLObjects(osg::State* state) const
{
    if (_shadowingStateSet.valid())
        _shadowingStateSet->releaseGLObjects(state);
    BOOST_FOREACH(const ref_ptr<Pass>& pass, passes)
    {
        pass->releaseGLObjects(state);
    }
    if (state == 0) {
        for (int i = 0; i < _contextMap.size(); ++i) {
            ContextInfo& info = _contextMap[i];
            Status oldVal = info.valid();
            info.valid.compareAndSwap(oldVal, UNKNOWN);
        }
    } else {
        ContextInfo& info = _contextMap[state->getContextID()];
        Status oldVal = info.valid();
        info.valid.compareAndSwap(oldVal, UNKNOWN);
    }
}

void Technique::setValidExpression(SGExpressionb* exp,
                                   const simgear::expression
                                   ::BindingLayout& layout)
{
    using namespace simgear::expression;
    _validExpression = exp;
    VariableBinding binding;
    if (layout.findBinding("__contextId", binding))
        _contextIdLocation = binding.location;
}

class GLVersionExpression : public SGExpression<float>
{
public:
    void eval(float& value, const expression::Binding*) const
    {
#ifdef TECHNIQUE_TEST_EXTENSIONS
        value = 1.1;
#else
        value = getGLVersionNumber();
#endif
    }
};

Expression* glVersionParser(const SGPropertyNode* exp,
                            expression::Parser* parser)
{
    return new GLVersionExpression();
}

expression::ExpParserRegistrar glVersionRegistrar("glversion", glVersionParser);

class ExtensionSupportedExpression
    : public GeneralNaryExpression<bool, int>
{
public:
    ExtensionSupportedExpression() {}
    ExtensionSupportedExpression(const string& extString)
        : _extString(extString)
    {
    }
    const string& getExtensionString() { return _extString; }
    void setExtensionString(const string& extString) { _extString = extString; }
    void eval(bool&value, const expression::Binding* b) const
    {
        int contextId = getOperand(0)->getValue(b);
        value = isGLExtensionSupported((unsigned)contextId, _extString.c_str());
    }
protected:
    string _extString;
};

Expression* extensionSupportedParser(const SGPropertyNode* exp,
                                     expression::Parser* parser)
{
    if (exp->getType() == props::STRING
        || exp->getType() == props::UNSPECIFIED) {
        ExtensionSupportedExpression* esp
            = new ExtensionSupportedExpression(exp->getStringValue());
        int location = parser->getBindingLayout().addBinding("__contextId",
                                                             expression::INT);
        VariableExpression<int>* contextExp
            = new VariableExpression<int>(location);
        esp->addOperand(contextExp);
        return esp;
    }
    throw expression::ParseError("extension-supported expression has wrong type");
}

expression::ExpParserRegistrar
extensionSupportedRegistrar("extension-supported", extensionSupportedParser);

void Technique::setGLExtensionsPred(float glVersion,
                                    const std::vector<std::string>& extensions)
{
    using namespace std;
    using namespace expression;
    BindingLayout layout;
    int contextLoc = layout.addBinding("__contextId", INT);
    VariableExpression<int>* contextExp
        = new VariableExpression<int>(contextLoc);
    SGExpression<bool>* versionTest
        = makePredicate<std::less_equal>(new SGConstExpression<float>(glVersion),
                        new GLVersionExpression);
    AndExpression* extensionsExp = 0;
    for (vector<string>::const_iterator itr = extensions.begin(),
             e = extensions.end();
         itr != e;
         ++itr) {
        if (!extensionsExp)
            extensionsExp = new AndExpression;
        ExtensionSupportedExpression* supported
            = new ExtensionSupportedExpression(*itr);
        supported->addOperand(contextExp);
        extensionsExp->addOperand(supported);
    }
    SGExpressionb* predicate = 0;
    if (extensionsExp) {
        OrExpression* orExp = new OrExpression;
        orExp->addOperand(versionTest);
        orExp->addOperand(extensionsExp);
        predicate = orExp;
    } else {
        predicate = versionTest;
    }
    setValidExpression(predicate, layout);
}

void Technique::refreshValidity()
{
    for (int i = 0; i < _contextMap.size(); ++i) {
        ContextInfo& info = _contextMap[i];
        Status oldVal = info.valid();
        // What happens if we lose the race here?
        info.valid.compareAndSwap(oldVal, UNKNOWN);
    }
}

bool Technique_writeLocalData(const Object& obj, osgDB::Output& fw)
{
    const Technique& tniq = static_cast<const Technique&>(obj);
    fw.indent() << "alwaysValid "
                << (tniq.getAlwaysValid() ? "TRUE\n" : "FALSE\n");
#if 0
    fw.indent() << "glVersion " << tniq.getGLVersion() << "\n";
#endif
    if (tniq.getShadowingStateSet()) {
        fw.indent() << "shadowingStateSet\n";
        fw.writeObject(*tniq.getShadowingStateSet());
    }
    fw.indent() << "num_passes " << tniq.passes.size() << "\n";
    BOOST_FOREACH(const ref_ptr<Pass>& pass, tniq.passes) {
        fw.writeObject(*pass);
    }
    return true;
}

namespace
{
osgDB::RegisterDotOsgWrapperProxy TechniqueProxy
(
    new Technique,
    "simgear::Technique",
    "Object simgear::Technique",
    0,
    &Technique_writeLocalData
    );
}
}