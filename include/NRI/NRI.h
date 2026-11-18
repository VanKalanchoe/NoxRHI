#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
    #if defined(NRI_STATIC_LIBRARY)
        #define NRI_API
    #elif defined(NRI_EXPORTS)
        #define NRI_API __declspec(dllexport)
    #else
        #define NRI_API __declspec(dllimport)
    #endif
    #define NRI_CALL __cdecl
#else
    #if defined(NRI_EXPORTS)
        #define NRI_API __attribute__((visibility("default")))
    #else
        #define NRI_API
    #endif
    #define NRI_CALL
#endif

#ifdef __cplusplus
    #define NRI_EXTERN_C_BEGIN extern "C" {
    #define NRI_EXTERN_C_END   }
#else
    #define NRI_EXTERN_C_BEGIN
    #define NRI_EXTERN_C_END
#endif

#include "NRITypes.h"
#include "NRIDevice.h"
#include "NRICommand.h"
#include "NRIResource.h"
#include "NRIPipeline.h"
