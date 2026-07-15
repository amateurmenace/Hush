// OpenNR — minimal OFX host harness: dlopens the built .ofx, runs the real
// load / describe / describeInContext / createInstance-free action sequence
// a host performs at scan time, and reports every parameter defined.
//
// This exists because a describe-time crash or exception is invisible in
// Resolve (the plugin just silently never appears). The property suite here
// is permissive (accepts every set, defaults every missing get), so this
// catches crashes and null derefs in plugin code — not host-specific
// property rejections.
//
// Build: c++ -O2 -std=c++14 -I../ofx/include test_describe.cpp -o test_describe
// Run:   ./test_describe ../plugin/OpenNR.ofx.bundle/Contents/MacOS/OpenNR.ofx

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxMessage.h"
#include "ofxInteract.h"

// ---------------------------------------------------------------------------
// property sets: every handle is a PropSet*; values are typed vectors
// ---------------------------------------------------------------------------
struct PropSet {
    std::string name;   // debugging
    std::map<std::string, std::vector<std::string>> s;
    std::map<std::string, std::vector<int>> i;
    std::map<std::string, std::vector<double>> d;
    std::map<std::string, std::vector<void*>> p;
};

static std::vector<PropSet*> g_allSets;
static PropSet* newSet(const std::string& n)
{
    PropSet* ps = new PropSet;
    ps->name = n;
    g_allSets.push_back(ps);
    return ps;
}

template <typename M, typename V>
static void put(M& m, const char* k, int idx, const V& v)
{
    auto& vec = m[k];
    if ((int)vec.size() <= idx) vec.resize(idx + 1);
    vec[idx] = v;
}

static OfxStatus pSetPointer(OfxPropertySetHandle h, const char* k, int i2, void* v)
{ put(((PropSet*)h)->p, k, i2, v); return kOfxStatOK; }
static OfxStatus pSetString(OfxPropertySetHandle h, const char* k, int i2, const char* v)
{ put(((PropSet*)h)->s, k, i2, std::string(v ? v : "")); return kOfxStatOK; }
static OfxStatus pSetDouble(OfxPropertySetHandle h, const char* k, int i2, double v)
{ put(((PropSet*)h)->d, k, i2, v); return kOfxStatOK; }
static OfxStatus pSetInt(OfxPropertySetHandle h, const char* k, int i2, int v)
{ put(((PropSet*)h)->i, k, i2, v); return kOfxStatOK; }
static OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* k, int n, void* const* v)
{ for (int j = 0; j < n; ++j) put(((PropSet*)h)->p, k, j, v[j]); return kOfxStatOK; }
static OfxStatus pSetStringN(OfxPropertySetHandle h, const char* k, int n, const char* const* v)
{ for (int j = 0; j < n; ++j) put(((PropSet*)h)->s, k, j, std::string(v[j] ? v[j] : "")); return kOfxStatOK; }
static OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* k, int n, const double* v)
{ for (int j = 0; j < n; ++j) put(((PropSet*)h)->d, k, j, v[j]); return kOfxStatOK; }
static OfxStatus pSetIntN(OfxPropertySetHandle h, const char* k, int n, const int* v)
{ for (int j = 0; j < n; ++j) put(((PropSet*)h)->i, k, j, v[j]); return kOfxStatOK; }

static OfxStatus pGetPointer(OfxPropertySetHandle h, const char* k, int i2, void** v)
{
    auto& m = ((PropSet*)h)->p;
    auto it = m.find(k);
    *v = (it != m.end() && (int)it->second.size() > i2) ? it->second[i2] : nullptr;
    return kOfxStatOK;
}
static OfxStatus pGetString(OfxPropertySetHandle h, const char* k, int i2, char** v)
{
    static thread_local std::vector<std::string> keep;
    auto& m = ((PropSet*)h)->s;
    auto it = m.find(k);
    keep.push_back((it != m.end() && (int)it->second.size() > i2) ? it->second[i2] : "");
    *v = const_cast<char*>(keep.back().c_str());
    return kOfxStatOK;
}
static OfxStatus pGetDouble(OfxPropertySetHandle h, const char* k, int i2, double* v)
{
    auto& m = ((PropSet*)h)->d;
    auto it = m.find(k);
    *v = (it != m.end() && (int)it->second.size() > i2) ? it->second[i2] : 0.0;
    return kOfxStatOK;
}
static OfxStatus pGetInt(OfxPropertySetHandle h, const char* k, int i2, int* v)
{
    auto& m = ((PropSet*)h)->i;
    auto it = m.find(k);
    *v = (it != m.end() && (int)it->second.size() > i2) ? it->second[i2] : 0;
    return kOfxStatOK;
}
static OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* k, int n, void** v)
{ for (int j = 0; j < n; ++j) pGetPointer(h, k, j, &v[j]); return kOfxStatOK; }
static OfxStatus pGetStringN(OfxPropertySetHandle h, const char* k, int n, char** v)
{ for (int j = 0; j < n; ++j) pGetString(h, k, j, &v[j]); return kOfxStatOK; }
static OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* k, int n, double* v)
{ for (int j = 0; j < n; ++j) pGetDouble(h, k, j, &v[j]); return kOfxStatOK; }
static OfxStatus pGetIntN(OfxPropertySetHandle h, const char* k, int n, int* v)
{ for (int j = 0; j < n; ++j) pGetInt(h, k, j, &v[j]); return kOfxStatOK; }
static OfxStatus pReset(OfxPropertySetHandle, const char*) { return kOfxStatOK; }
static OfxStatus pGetDimension(OfxPropertySetHandle h, const char* k, int* n)
{
    PropSet* ps = (PropSet*)h;
    size_t d = 0;
    if (ps->s.count(k)) d = ps->s[k].size();
    else if (ps->i.count(k)) d = ps->i[k].size();
    else if (ps->d.count(k)) d = ps->d[k].size();
    else if (ps->p.count(k)) d = ps->p[k].size();
    *n = (int)d;
    return kOfxStatOK;
}

static OfxPropertySuiteV1 g_propSuite = {
    pSetPointer, pSetString, pSetDouble, pSetInt,
    pSetPointerN, pSetStringN, pSetDoubleN, pSetIntN,
    pGetPointer, pGetString, pGetDouble, pGetInt,
    pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN,
    pReset, pGetDimension
};

// ---------------------------------------------------------------------------
// param suite: paramDefine records name+type and hands back a property set
// ---------------------------------------------------------------------------
struct ParamSetRec {
    PropSet props;                                     // the param SET's props
    std::vector<std::pair<std::string, PropSet*>> params;
};
static ParamSetRec g_params;

static OfxStatus prmDefine(OfxParamSetHandle, const char* type, const char* name,
                           OfxPropertySetHandle* props)
{
    PropSet* ps = newSet(std::string("param:") + name);
    put(ps->s, kOfxParamPropType, 0, std::string(type));
    put(ps->s, kOfxPropName, 0, std::string(name));
    g_params.params.push_back({ std::string(name) + " [" + type + "]", ps });
    if (props) *props = (OfxPropertySetHandle)ps;
    return kOfxStatOK;
}
static OfxStatus prmGetHandle(OfxParamSetHandle, const char* name, OfxParamHandle* param,
                              OfxPropertySetHandle* props)
{
    for (auto& pr : g_params.params)
        if (pr.first.compare(0, strlen(name), name) == 0 &&
            pr.first[strlen(name)] == ' ') {
            if (param) *param = (OfxParamHandle)pr.second;
            if (props) *props = (OfxPropertySetHandle)pr.second;
            return kOfxStatOK;
        }
    return kOfxStatErrUnknown;
}
static OfxStatus prmSetGetPropertySet(OfxParamSetHandle, OfxPropertySetHandle* props)
{ *props = (OfxPropertySetHandle)&g_params.props; return kOfxStatOK; }
static OfxStatus prmGetPropertySet(OfxParamHandle param, OfxPropertySetHandle* props)
{ *props = (OfxPropertySetHandle)param; return kOfxStatOK; }
static OfxStatus prmGetValue(OfxParamHandle, ...) { return kOfxStatOK; }
static OfxStatus prmGetValueAtTime(OfxParamHandle, OfxTime, ...) { return kOfxStatOK; }
static OfxStatus prmGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatOK; }
static OfxStatus prmGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatOK; }
static OfxStatus prmSetValue(OfxParamHandle, ...) { return kOfxStatOK; }
static OfxStatus prmSetValueAtTime(OfxParamHandle, OfxTime, ...) { return kOfxStatOK; }
static OfxStatus prmGetNumKeys(OfxParamHandle, unsigned int* n) { *n = 0; return kOfxStatOK; }
static OfxStatus prmGetKeyTime(OfxParamHandle, unsigned int, OfxTime* t) { *t = 0; return kOfxStatOK; }
static OfxStatus prmGetKeyIndex(OfxParamHandle, OfxTime, int, int* i2) { *i2 = 0; return kOfxStatOK; }
static OfxStatus prmDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
static OfxStatus prmDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
static OfxStatus prmCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
static OfxStatus prmEditBegin(OfxParamSetHandle, const char* name)
{ printf("    paramEditBegin(\"%s\")\n", name); return kOfxStatOK; }
static OfxStatus prmEditEnd(OfxParamSetHandle) { printf("    paramEditEnd()\n"); return kOfxStatOK; }

static OfxParameterSuiteV1 g_paramSuite = {
    prmDefine, prmGetHandle, prmSetGetPropertySet, prmGetPropertySet,
    prmGetValue, prmGetValueAtTime, prmGetDerivative, prmGetIntegral,
    prmSetValue, prmSetValueAtTime, prmGetNumKeys, prmGetKeyTime,
    prmGetKeyIndex, prmDeleteKey, prmDeleteAllKeys, prmCopy,
    prmEditBegin, prmEditEnd
};

// ---------------------------------------------------------------------------
// image effect suite (describe-time subset)
// ---------------------------------------------------------------------------
static PropSet* g_effectProps = nullptr;

static OfxStatus effGetPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle* props)
{ *props = (OfxPropertySetHandle)h; return kOfxStatOK; }
static OfxStatus effGetParamSet(OfxImageEffectHandle, OfxParamSetHandle* ps)
{ *ps = (OfxParamSetHandle)&g_params; return kOfxStatOK; }
static OfxStatus effClipDefine(OfxImageEffectHandle, const char* name, OfxPropertySetHandle* props)
{
    PropSet* ps = newSet(std::string("clip:") + name);
    printf("    clipDefine(\"%s\")\n", name);
    if (props) *props = (OfxPropertySetHandle)ps;
    return kOfxStatOK;
}
static OfxStatus effClipGetHandle(OfxImageEffectHandle, const char* name, OfxImageClipHandle* clip,
                                  OfxPropertySetHandle* props)
{
    PropSet* ps = newSet(std::string("cliph:") + name);
    if (clip) *clip = (OfxImageClipHandle)ps;
    if (props) *props = (OfxPropertySetHandle)ps;
    return kOfxStatOK;
}
static OfxStatus effClipGetPropertySet(OfxImageClipHandle h, OfxPropertySetHandle* props)
{ *props = (OfxPropertySetHandle)h; return kOfxStatOK; }
static OfxStatus effClipGetImage(OfxImageClipHandle, OfxTime, const OfxRectD*, OfxPropertySetHandle* img)
{ *img = nullptr; return kOfxStatFailed; }
static OfxStatus effClipReleaseImage(OfxPropertySetHandle) { return kOfxStatOK; }
static OfxStatus effClipGetRegionOfDefinition(OfxImageClipHandle, OfxTime, OfxRectD* rod)
{ rod->x1 = rod->y1 = 0; rod->x2 = rod->y2 = 1; return kOfxStatOK; }
static int effAbort(OfxImageEffectHandle) { return 0; }
static OfxStatus effImageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h)
{ *h = (OfxImageMemoryHandle)malloc(n); return kOfxStatOK; }
static OfxStatus effImageMemoryFree(OfxImageMemoryHandle h) { free((void*)h); return kOfxStatOK; }
static OfxStatus effImageMemoryLock(OfxImageMemoryHandle h, void** p2) { *p2 = (void*)h; return kOfxStatOK; }
static OfxStatus effImageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

static OfxImageEffectSuiteV1 g_effectSuite = {
    effGetPropertySet, effGetParamSet, effClipDefine, effClipGetHandle,
    effClipGetPropertySet, effClipGetImage, effClipReleaseImage,
    effClipGetRegionOfDefinition, effAbort,
    effImageMemoryAlloc, effImageMemoryFree, effImageMemoryLock, effImageMemoryUnlock
};

// memory / thread / message suites
static OfxStatus memAlloc(void*, size_t n, void** p2) { *p2 = malloc(n); return kOfxStatOK; }
static OfxStatus memFree(void* p2) { free(p2); return kOfxStatOK; }
static OfxMemorySuiteV1 g_memSuite = { memAlloc, memFree };

static OfxStatus thrMultiThread(OfxThreadFunctionV1 f, unsigned int n, void* a)
{ f(0, 1, a); (void)n; return kOfxStatOK; }
static OfxStatus thrNumCPUs(unsigned int* n) { *n = 1; return kOfxStatOK; }
static OfxStatus thrIndex(unsigned int* i2) { *i2 = 0; return kOfxStatOK; }
static int thrIsSpawned(void) { return 0; }
static OfxStatus thrMutexCreate(OfxMutexHandle* m, int) { *m = (OfxMutexHandle)1; return kOfxStatOK; }
static OfxStatus thrMutexDestroy(OfxMutexHandle) { return kOfxStatOK; }
static OfxStatus thrMutexLock(OfxMutexHandle) { return kOfxStatOK; }
static OfxStatus thrMutexUnLock(OfxMutexHandle) { return kOfxStatOK; }
static OfxStatus thrMutexTryLock(OfxMutexHandle) { return kOfxStatOK; }
static OfxMultiThreadSuiteV1 g_threadSuite = {
    thrMultiThread, thrNumCPUs, thrIndex, thrIsSpawned,
    thrMutexCreate, thrMutexDestroy, thrMutexLock, thrMutexUnLock, thrMutexTryLock
};

static OfxStatus msgMessage(void*, const char* type, const char* id, const char* fmt, ...)
{ printf("    message(%s,%s): %s\n", type ? type : "", id ? id : "", fmt ? fmt : ""); return kOfxStatOK; }
static OfxMessageSuiteV1 g_msgSuite = { msgMessage };

// interact suite (mandatory when the host advertises overlay support)
static OfxStatus intSwapBuffers(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus intRedraw(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus intGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* p2)
{ *p2 = (OfxPropertySetHandle)h; return kOfxStatOK; }
static OfxInteractSuiteV1 g_interactSuite = { intSwapBuffers, intRedraw, intGetPropertySet };

// host
static PropSet* g_hostProps = nullptr;
static const void* fetchSuite(OfxPropertySetHandle, const char* name, int ver)
{
    if (!strcmp(name, kOfxPropertySuite)) return &g_propSuite;
    if (!strcmp(name, kOfxParameterSuite)) return &g_paramSuite;
    if (!strcmp(name, kOfxImageEffectSuite)) return &g_effectSuite;
    if (!strcmp(name, kOfxMemorySuite)) return &g_memSuite;
    if (!strcmp(name, kOfxMultiThreadSuite)) return &g_threadSuite;
    if (!strcmp(name, kOfxMessageSuite)) return &g_msgSuite;
    if (!strcmp(name, kOfxInteractSuite)) return &g_interactSuite;
    printf("    (suite not provided: %s v%d)\n", name, ver);
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.ofx>\n", argv[0]); return 2; }

    void* dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "dlopen FAILED: %s\n", dlerror()); return 1; }

    typedef int (*GetNumF)(void);
    typedef OfxPlugin* (*GetPluginF)(int);
    GetNumF getNum = (GetNumF)dlsym(dl, "OfxGetNumberOfPlugins");
    GetPluginF getPlugin = (GetPluginF)dlsym(dl, "OfxGetPlugin");
    if (!getNum || !getPlugin) { fprintf(stderr, "entry points missing\n"); return 1; }

    const int n = getNum();
    printf("OfxGetNumberOfPlugins = %d\n", n);
    for (int idx = 0; idx < n; ++idx) {
        OfxPlugin* plug = getPlugin(idx);
        printf("plugin[%d]: id=%s api=%s apiVer=%d version=%d.%d\n", idx,
               plug->pluginIdentifier, plug->pluginApi, plug->apiVersion,
               plug->pluginVersionMajor, plug->pluginVersionMinor);

        // host description
        g_hostProps = newSet("host");
        put(g_hostProps->s, kOfxPropName, 0, std::string("org.opennr.testhost"));
        put(g_hostProps->s, kOfxPropLabel, 0, std::string("OpenNR describe harness"));
        put(g_hostProps->i, kOfxImageEffectHostPropIsBackground, 0, 0);
        put(g_hostProps->i, kOfxImageEffectPropSupportsOverlays, 0, 1);
        put(g_hostProps->i, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
        put(g_hostProps->i, kOfxImageEffectPropSupportsTiles, 0, 1);
        put(g_hostProps->i, kOfxImageEffectPropTemporalClipAccess, 0, 1);
        put(g_hostProps->s, kOfxImageEffectPropSupportedComponents, 0, std::string(kOfxImageComponentRGBA));
        put(g_hostProps->s, kOfxImageEffectPropSupportedContexts, 0, std::string(kOfxImageEffectContextFilter));
        put(g_hostProps->s, kOfxImageEffectPropSupportedContexts, 1, std::string(kOfxImageEffectContextGeneral));
        put(g_hostProps->i, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
        put(g_hostProps->i, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
        put(g_hostProps->i, kOfxImageEffectPropSetableFrameRate, 0, 0);
        put(g_hostProps->i, kOfxImageEffectPropSetableFielding, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropSupportsCustomInteract, 0, 1);
        put(g_hostProps->i, kOfxParamHostPropSupportsStringAnimation, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropSupportsChoiceAnimation, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropSupportsBooleanAnimation, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropSupportsCustomAnimation, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropMaxParameters, 0, -1);
        put(g_hostProps->i, kOfxParamHostPropMaxPages, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropPageRowColumnCount, 0, 0);
        put(g_hostProps->i, kOfxParamHostPropPageRowColumnCount, 1, 0);

        static OfxHost host;
        host.host = (OfxPropertySetHandle)g_hostProps;
        host.fetchSuite = fetchSuite;
        plug->setHost(&host);

        OfxStatus st = plug->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
        printf("  kOfxActionLoad -> %d\n", st);
        if (st != kOfxStatOK && st != kOfxStatReplyDefault) return 1;

        g_effectProps = newSet("effectDescriptor");
        st = plug->mainEntry(kOfxActionDescribe, (void*)g_effectProps, nullptr, nullptr);
        printf("  kOfxActionDescribe -> %d\n", st);
        if (st != kOfxStatOK && st != kOfxStatReplyDefault) return 1;

        const char* contexts[2] = { kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral };
        for (int c = 0; c < 2; ++c) {
            g_params.params.clear();
            PropSet* ctxDesc = newSet("contextDescriptor");
            PropSet* inArgs = newSet("inArgs");
            put(inArgs->s, kOfxImageEffectPropContext, 0, std::string(contexts[c]));
            st = plug->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 (void*)ctxDesc, (OfxPropertySetHandle)inArgs, nullptr);
            printf("  describeInContext(%s) -> %d, %zu params defined\n",
                   contexts[c], st, g_params.params.size());
            if (st != kOfxStatOK && st != kOfxStatReplyDefault) return 1;
            if (c == 0)
                for (auto& pr : g_params.params)
                    printf("      %s\n", pr.first.c_str());
        }

        st = plug->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        printf("  kOfxActionUnload -> %d\n", st);
    }
    printf("DESCRIBE HARNESS: ALL ACTIONS COMPLETED WITHOUT CRASH\n");
    return 0;
}
