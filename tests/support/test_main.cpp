#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <typeinfo>

#if defined(_WIN32) && defined(_DEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <crtdbg.h>
#include <dbghelp.h>
#endif

#include "test_harness.h"

#if defined(_WIN32) && defined(_DEBUG)
namespace {
// TEMPORARY CI crash diagnostics: remove after the Windows failures are understood.
void print_symbol_stack(CONTEXT* context = nullptr) noexcept {
    HANDLE process = GetCurrentProcess();
    if (!SymInitialize(process, nullptr, TRUE)) {
        std::fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
    }
    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

    if (context == nullptr) {
        void* frames[64]{};
        const auto count = CaptureStackBackTrace(0,
            static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])), frames, nullptr);
        for (USHORT index = 0; index < count; ++index) {
            const auto address = reinterpret_cast<DWORD64>(frames[index]);
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol)) {
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD line_displacement = 0;
                if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
                    std::fprintf(stderr, "  #%u %s + 0x%llx (%s:%lu)\n", static_cast<unsigned>(index),
                        symbol->Name, displacement, line.FileName, line.LineNumber);
                } else {
                    std::fprintf(stderr, "  #%u %s + 0x%llx\n", static_cast<unsigned>(index), symbol->Name,
                        displacement);
                }
            } else {
                std::fprintf(stderr, "  #%u 0x%llx\n", static_cast<unsigned>(index), address);
            }
        }
        return;
    }

    STACKFRAME64 frame{};
#if defined(_M_X64) || defined(_M_AMD64)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context->Rip;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrStack.Offset = context->Rsp;
#elif defined(_M_IX86)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context->Eip;
    frame.AddrFrame.Offset = context->Ebp;
    frame.AddrStack.Offset = context->Esp;
#elif defined(_M_ARM64)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context->Pc;
    frame.AddrFrame.Offset = context->Fp;
    frame.AddrStack.Offset = context->Sp;
#else
    std::fprintf(stderr, "  SEH stack walking unsupported for this architecture\n");
    return;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    for (unsigned index = 0; index < 64 && frame.AddrPC.Offset != 0; ++index) {
        const auto address = frame.AddrPC.Offset;
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
                std::fprintf(stderr, "  #%u %s + 0x%llx (%s:%lu)\n", index, symbol->Name,
                    displacement, line.FileName, line.LineNumber);
            } else {
                std::fprintf(stderr, "  #%u %s + 0x%llx\n", index, symbol->Name, displacement);
            }
        } else {
            std::fprintf(stderr, "  #%u 0x%llx\n", index, address);
        }
        if (!StackWalk64(machine, process, GetCurrentThread(), &frame, context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
    }
}

void terminate_with_diagnostics() noexcept {
    std::fprintf(stderr, "std::terminate invoked\n");
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "exception type: %s\nwhat(): %s\n", typeid(error).name(),
                error.what());
        } catch (...) {
            std::fprintf(stderr, "exception type: non-std or unknown\n");
        }
    } else {
        std::fprintf(stderr, "no active exception\n");
    }
    print_symbol_stack();
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception) noexcept {
    const DWORD code = exception != nullptr && exception->ExceptionRecord != nullptr
                           ? exception->ExceptionRecord->ExceptionCode
                           : 0;
    std::fprintf(stderr, "unhandled SEH exception: code 0x%08lx\n", code);
    print_symbol_stack(exception != nullptr ? exception->ContextRecord : nullptr);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace
#endif

int main() {
#if defined(_WIN32) && defined(_DEBUG)
    std::set_terminate(terminate_with_diagnostics);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif

    using namespace ruvia::testing;
    int totalFailures = 0;
    int failedCases = 0;
    std::size_t executedCases = 0;
    auto& cases = registry();
    const char* filter = std::getenv("RUVIA_TEST_FILTER");
    const char* firstText = std::getenv("RUVIA_TEST_FIRST");
    const char* lastText = std::getenv("RUVIA_TEST_LAST");
    const auto first = firstText != nullptr
                           ? static_cast<std::size_t>(std::strtoull(firstText, nullptr, 10))
                           : std::size_t{0};
    const auto last = lastText != nullptr
                          ? static_cast<std::size_t>(std::strtoull(lastText, nullptr, 10))
                          : cases.size();
    std::size_t caseIndex = 0;
    for (auto& c : cases) {
        if (caseIndex < first || caseIndex > last) {
            ++caseIndex;
            continue;
        }
        if (filter != nullptr) {
            const std::string_view filters(filter);
            bool matched = false;
            for (std::size_t begin = 0; begin <= filters.size();) {
                const auto end = filters.find('|', begin);
                const auto token = filters.substr(
                    begin, end == std::string_view::npos ? filters.size() - begin : end - begin);
                if (!token.empty() &&
                    std::string_view(c.name).contains(token)) {
                    matched = true;
                    break;
                }
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            if (!matched) {
                ++caseIndex;
                continue;
            }
        }
        ++executedCases;
        TestContext ctx;
        ctx.current = c.name;
        std::printf("[ RUN ] %s (#%zu)\n", c.name, caseIndex);
        std::fflush(stdout);
        c.fn(ctx);
        if (ctx.failures == 0) {
            std::printf("[ ok ] %s\n", c.name);
        } else {
            std::printf("[FAIL] %s (%d checks failed)\n", c.name, ctx.failures);
            ++failedCases;
        }
        std::fflush(stdout);
        totalFailures += ctx.failures;
        ++caseIndex;
    }
    std::printf("\n%zu tests, %d failed cases, %d failed checks\n", executedCases, failedCases,
        totalFailures);
    return totalFailures == 0 ? 0 : 1;
}
