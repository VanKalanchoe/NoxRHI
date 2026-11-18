#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef struct NriShaderProgressCallbacks
{
    void (*onCompile)(const char* sourcePath, void* userData);
    void (*onPipelineCreated)(void* userData);
    void (*onIdle)(void* userData);
    void* userData;
} NriShaderProgressCallbacks;

NRI_API NriResult NRI_CALL nriCreateSlangCompiler(NriShaderCompiler* outCompiler);
NRI_API void NRI_CALL nriDestroyShaderCompiler(NriShaderCompiler compiler);

NRI_API void NRI_CALL nriShaderCompilerSetTargetAPI(NriShaderCompiler compiler, NriGraphicsAPI api);
NRI_API NriGraphicsAPI NRI_CALL nriShaderCompilerGetTargetAPI(NriShaderCompiler compiler);
NRI_API void NRI_CALL nriShaderCompilerSetSearchPaths(NriShaderCompiler compiler, const char* const* paths, uint32_t pathCount);
NRI_API void NRI_CALL nriShaderCompilerAddSearchPath(NriShaderCompiler compiler, const char* path);
NRI_API bool NRI_CALL nriShaderCompilerCanCompile(NriShaderCompiler compiler);
NRI_API uint64_t NRI_CALL nriShaderCompilerSourceHash(NriShaderCompiler compiler, const char* path);

// Returns true on success; caller supplies a buffer or passes outData = NULL to query outSize,
// or uses nriFreeCompiledShader() on an allocated buffer via nriShaderCompilerCompileAlloc().
NRI_API NriResult NRI_CALL nriShaderCompilerCompile(NriShaderCompiler compiler, const char* path, char** outBytes, size_t* outSize);
NRI_API void NRI_CALL nriFreeCompiledShaderBytes(char* bytes);

NRI_API void NRI_CALL nriSetShaderProgressCallbacks(const NriShaderProgressCallbacks* callbacks);

NRI_EXTERN_C_END
