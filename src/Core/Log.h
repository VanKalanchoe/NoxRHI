#pragma once
#include <cstdint>
#include <cstdio>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace NRI
{
    enum class LogLevel : uint8_t
    {
        Info = 0,
        Warn = 1,
        Error = 2
    };

    using LogCallbackFn = void (*)(LogLevel level, const char* message, void* userData);

    namespace detail
    {
        inline std::mutex& logMutex()
        {
            static std::mutex m;
            return m;
        }

        inline LogCallbackFn& logCallback()
        {
            static LogCallbackFn cb = nullptr;
            return cb;
        }

        inline void*& logUserData()
        {
            static void* ud = nullptr;
            return ud;
        }

        inline thread_local std::string t_lastError;
    }

    inline void SetLogCallback(LogCallbackFn callback, void* userData)
    {
        std::lock_guard lock(detail::logMutex());
        detail::logCallback() = callback;
        detail::logUserData() = userData;
    }

    inline void SetLastError(std::string_view message)
    {
        detail::t_lastError.assign(message.data(), message.size());
    }

    inline const char* GetLastError()
    {
        return detail::t_lastError.c_str();
    }

    inline void LogMessage(LogLevel level, std::string_view message)
    {
        LogCallbackFn cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard lock(detail::logMutex());
            cb = detail::logCallback();
            ud = detail::logUserData();
        }
        if (cb)
        {
            std::string nullTerminated(message);
            cb(level, nullTerminated.c_str(), ud);
            return;
        }
        const char* prefix = level == LogLevel::Error ? "[NoxRHI][ERROR]" :
                             level == LogLevel::Warn  ? "[NoxRHI][WARN]" : "[NoxRHI][INFO]";
        std::fprintf(stderr, "%s %.*s\n", prefix, static_cast<int>(message.size()), message.data());
    }
}

#define NRI_LOG_INFO(...)  ::NRI::LogMessage(::NRI::LogLevel::Info,  ::std::format(__VA_ARGS__))
#define NRI_LOG_WARN(...)  ::NRI::LogMessage(::NRI::LogLevel::Warn,  ::std::format(__VA_ARGS__))
#define NRI_LOG_ERROR(...) ::NRI::LogMessage(::NRI::LogLevel::Error, ::std::format(__VA_ARGS__))

#ifndef NDEBUG
#define NRI_ASSERT(cond, ...)                                                              \
    do                                                                                     \
    {                                                                                      \
        if (!(cond))                                                                       \
        {                                                                                  \
            const ::std::string _nriAssertMsg = ::std::format(__VA_ARGS__);                \
            ::NRI::LogMessage(::NRI::LogLevel::Error, "Assertion failed: " + _nriAssertMsg); \
            throw ::std::runtime_error("NRI assertion failed: " + _nriAssertMsg);          \
        }                                                                                  \
    } while (false)
#else
#define NRI_ASSERT(cond, ...) ((void)0)
#endif
