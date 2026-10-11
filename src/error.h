#include <windows.h>
#include <cstdio>
#include <type_traits>

template <typename T, typename CHAR_T>
inline constexpr bool is_char_impl_v =
    std::is_same_v<std::remove_cvref_t<T>, const CHAR_T *> ||
    std::is_same_v<std::remove_cvref_t<T>, CHAR_T *> ||
    (std::is_array_v<std::remove_reference_t<T>> && std::is_same_v<std::remove_cv_t<std::remove_all_extents_t<std::remove_reference_t<T>>>, CHAR_T>);

template <typename T>
inline constexpr bool is_char_v = is_char_impl_v<T, char>;

template <typename T>
inline constexpr bool is_wchar_v = is_char_impl_v<T, wchar_t>;

template <typename... ARGS>
void Throw(const char *messageFormat, ARGS &&...args)
{
    static_assert((!is_wchar_v<ARGS> && ...), "passing 'wchar_t' args to 'char' message format is not allowed");

    char message[512];
    std::snprintf(message, 512, messageFormat, std::forward<ARGS>(args)...);

    OutputDebugStringA(message);

    __debugbreak();

    MessageBoxA(nullptr, message, "Error", MB_ICONERROR | MB_OK);

    ExitProcess(1);
}

template <typename... ARGS>
void Throw(const wchar_t *messageFormat, ARGS &&...args)
{
    static_assert((!is_char_v<ARGS> && ...), "passing 'char' args to 'wchar_t' message format is not allowed");

    wchar_t message[512];
    std::swprintf(message, 512, messageFormat, std::forward<ARGS>(args)...);

    OutputDebugStringW(message);

    __debugbreak();

    MessageBoxW(nullptr, message, L"Error", MB_ICONERROR | MB_OK);

    ExitProcess(1);
}

#define XSTR(a) STR(a)
#define STR(a) #a

#define LINE_AS_STR XSTR(__LINE__)
#define FILE_AND_LINE __FILE__ ":" LINE_AS_STR

#define ASSERT(condition, message, ...)                                                            \
    if (!(condition))                                                                              \
    {                                                                                              \
        const auto *fullMessage = (message "\n\ncondition: " #condition "\nfile: " FILE_AND_LINE); \
        Throw(fullMessage, __VA_ARGS__);                                                           \
    }

#define SANITY_CHECK(condition, ...) ASSERT(condition, "something went wrong", __VA_ARGS__)

#define WIN_CALL(...)          \
    do                             \
    {                              \
        auto hr = (__VA_ARGS__);   \
        SANITY_CHECK(!FAILED(hr)); \
    } while (false)