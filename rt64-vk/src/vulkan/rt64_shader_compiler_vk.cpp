#include "rt64_shader_compiler_vk.h"

#include <cstring>
#include <dlfcn.h>

#define __EMULATE_UUID 1
#include <dxc/dxcapi.h>

namespace RT64 {

namespace {

std::wstring widen(const std::string &s) {
    /* The Linux DXC build uses native wchar_t (typedef wchar_t WCHAR), not
       the -fshort-wchar UTF-16 convention Windows uses, so a plain widening
       is correct here. Shader source and flags are ASCII. */
    return std::wstring(s.begin(), s.end());
}

/* Must match cmake/CompileShaders.cmake exactly. Passed in as compile
   definitions so the two cannot drift: runtime-compiled hit groups share a
   descriptor set with build-time shaders, and a different shift would bind
   them to the wrong slots with no error. */
#ifndef RT64_VK_U_SHIFT
#   error "RT64_VK_*_SHIFT must be supplied by the build"
#endif

} /* namespace */

bool ShaderCompilerVK::initialize(const std::string &explicitPath,
                                  std::string &error) {
    if (compiler != nullptr) {
        return true;
    }

    std::vector<std::string> candidates;
    if (!explicitPath.empty()) {
        candidates.push_back(explicitPath);
    }
    if (const char *env = std::getenv("RT64_DXC_LIB")) {
        candidates.push_back(env);
    }
    candidates.push_back("./libdxcompiler.so");
    candidates.push_back("libdxcompiler.so");

    for (const std::string &path : candidates) {
        libraryHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (libraryHandle != nullptr) {
            libraryPath = path;
            break;
        }
    }
    if (libraryHandle == nullptr) {
        error = "could not load libdxcompiler.so. Set RT64_DXC_LIB or place it "
                "beside the executable (tools/get_dxc.sh installs one).";
        return false;
    }

    auto createInstance =
        (DxcCreateInstanceProc)dlsym(libraryHandle, "DxcCreateInstance");
    if (createInstance == nullptr) {
        error = libraryPath + " has no DxcCreateInstance";
        return false;
    }

    IDxcCompiler3 *c = nullptr;
    if (createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&c)) < 0 || c == nullptr) {
        error = "DxcCreateInstance(IDxcCompiler3) failed";
        return false;
    }
    IDxcUtils *u = nullptr;
    if (createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&u)) < 0 || u == nullptr) {
        c->Release();
        error = "DxcCreateInstance(IDxcUtils) failed";
        return false;
    }
    compiler = c;
    utils = u;
    return true;
}

bool ShaderCompilerVK::compile(const std::string &source,
                               const std::string &entryPoint,
                               const std::string &profile,
                               std::vector<uint32_t> &spirv,
                               std::string &error) {
    if (compiler == nullptr) {
        error = "shader compiler not initialised";
        return false;
    }

    /* Held by value so the wide strings outlive the Compile call. */
    std::vector<std::wstring> argStorage;
    argStorage.push_back(L"-T");
    argStorage.push_back(widen(profile));
    if (!entryPoint.empty()) {
        argStorage.push_back(L"-E");
        argStorage.push_back(widen(entryPoint));
    }
    argStorage.push_back(L"-spirv");
    argStorage.push_back(L"-HV");
    argStorage.push_back(L"2018");
    argStorage.push_back(L"-fspv-target-env=vulkan1.3");
    argStorage.push_back(L"-fvk-u-shift");
    argStorage.push_back(widen(std::to_string(RT64_VK_U_SHIFT)));
    argStorage.push_back(L"0");
    argStorage.push_back(L"-fvk-t-shift");
    argStorage.push_back(widen(std::to_string(RT64_VK_T_SHIFT)));
    argStorage.push_back(L"0");
    argStorage.push_back(L"-fvk-b-shift");
    argStorage.push_back(widen(std::to_string(RT64_VK_B_SHIFT)));
    argStorage.push_back(L"0");
    argStorage.push_back(L"-fvk-s-shift");
    argStorage.push_back(widen(std::to_string(RT64_VK_S_SHIFT)));
    argStorage.push_back(L"0");

    std::vector<const wchar_t *> args;
    args.reserve(argStorage.size());
    for (const std::wstring &a : argStorage) {
        args.push_back(a.c_str());
    }

    DxcBuffer buffer = {};
    buffer.Ptr = source.data();
    buffer.Size = source.size();
    buffer.Encoding = DXC_CP_UTF8;

    IDxcResult *result = nullptr;
    IDxcCompiler3 *c = (IDxcCompiler3 *)compiler;
    HRESULT hr = c->Compile(&buffer, args.data(), (UINT32)args.size(), nullptr,
                            IID_PPV_ARGS(&result));
    if (hr < 0 || result == nullptr) {
        error = "IDxcCompiler3::Compile failed to run";
        return false;
    }

    HRESULT status = 0;
    result->GetStatus(&status);
    if (status < 0) {
        IDxcBlobUtf8 *errors = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        error = (errors != nullptr && errors->GetStringLength() > 0)
                    ? std::string(errors->GetStringPointer(),
                                  errors->GetStringLength())
                    : "shader compilation failed with no diagnostic";
        if (errors != nullptr) { errors->Release(); }
        result->Release();
        return false;
    }

    IDxcBlob *object = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (object == nullptr || object->GetBufferSize() == 0) {
        error = "compilation produced no SPIR-V";
        if (object != nullptr) { object->Release(); }
        result->Release();
        return false;
    }

    const size_t bytes = object->GetBufferSize();
    if ((bytes % 4) != 0) {
        error = "SPIR-V output is not a whole number of words";
        object->Release();
        result->Release();
        return false;
    }
    spirv.resize(bytes / 4);
    std::memcpy(spirv.data(), object->GetBufferPointer(), bytes);

    object->Release();
    result->Release();
    return true;
}

ShaderCompilerVK::~ShaderCompilerVK() {
    if (utils != nullptr)    { ((IDxcUtils *)utils)->Release(); }
    if (compiler != nullptr) { ((IDxcCompiler3 *)compiler)->Release(); }
    if (libraryHandle != nullptr) { dlclose(libraryHandle); }
}

} /* namespace RT64 */
