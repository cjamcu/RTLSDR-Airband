# AGENTS.md - RTLSDR-Airband

Guidance for AI coding agents working on the RTLSDR-Airband codebase.

## Project Overview

C++ application receiving analog radio voice channels from SDRs (RTL-SDR, SoapySDR, Mirics) to produce audio streams for services like LiveATC.net. Licensed under GPLv2.

## Build Commands

```bash
# Configure Debug build with unit tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNITTESTS=TRUE

# Build
cmake --build build -j4

# Install
sudo cmake --install build
```

### Key Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_UNITTESTS` | OFF | Enable GoogleTest unit tests |
| `NFM` | OFF | Enable narrow FM channel support |
| `DEBUG_SQUELCH` | OFF | Enable squelch debugging output |

## Testing

Unit tests use GoogleTest (fetched automatically during build).

```bash
# Run all tests
./build/src/unittests

# Run specific test by name
./build/src/unittests --gtest_filter=SquelchTest.normal_operation

# Run all tests in a test suite
./build/src/unittests --gtest_filter=SquelchTest.*

# Run tests matching a pattern
./build/src/unittests --gtest_filter=*ctcss*

# CTest integration
cd build && ctest --output-on-failure
```

## Code Formatting

Uses `clang-format-14` with Chromium style. Enforced in CI.

```bash
./scripts/reformat_code
```

Config (`.clang-format`): Chromium style, 4-space indent, 200-char column limit.

## Code Style Guidelines

### C++ Standard
- C++11 for main code, C++14 for unit tests
- Compiler flags: `-Wall -Wextra -Wshadow -Werror` (warnings as errors)

### Include Order
1. Corresponding header (for .cpp files)
2. C system headers (`<errno.h>`, `<string.h>`)
3. C++ standard headers (`<algorithm>`, `<cassert>`)
4. Third-party headers (`<libconfig.h++>`, `<lame/lame.h>`)
5. Project headers (`"config.h"`, `"logging.h"`)

```cpp
#include "squelch.h"

#include <errno.h>
#include <algorithm>
#include <libconfig.h++>
#include "logging.h"
```

### Naming Conventions

| Element | Style | Example |
|---------|-------|---------|
| Classes | PascalCase | `Squelch`, `NotchFilter` |
| Functions | snake_case | `process_raw_sample()` |
| Member variables | snake_case + trailing `_` | `noise_floor_`, `open_count_` |
| Constants/Macros | UPPER_SNAKE_CASE | `WAVE_RATE`, `MIN_BUF_SIZE` |
| Enums | PascalCase type, UPPER_SNAKE_CASE values | `enum State { CLOSED, OPENING }` |
| Structs | snake_case with `_t` suffix | `device_t`, `channel_t` |

### Header Guards
```cpp
#ifndef _SQUELCH_H
#define _SQUELCH_H
// ...
#endif /* _SQUELCH_H */
```

### Class Structure
1. Public section first (constructors, public methods)
2. Private section (enums, structs, member variables, private methods)
3. Conditional debug members at end with `#ifdef DEBUG`

### Function Parameters
- Use `const` references for input: `const float& sample`
- Use `void` explicitly for no-parameter functions: `bool is_open(void) const`

### Memory & Error Handling
- Use `XCALLOC(nmemb, size)` and `XREALLOC(ptr, size)` for allocation
- Use `log(priority, format, ...)` for logging (`LOG_ERR`, `LOG_WARNING`, `LOG_INFO`)
- Use `debug_print(fmt, ...)` for debug output (only with `-DDEBUG`)
- Use `UNUSED(x)` macro to suppress unused parameter warnings
- Use `assert()` for internal invariants

### Conditional Compilation Guards
- `DEBUG` - Debug logging
- `DEBUG_SQUELCH` - Squelch debugging
- `NFM` - Narrow FM support
- `WITH_PULSEAUDIO`, `WITH_RTLSDR`, `WITH_SOAPYSDR`, `WITH_MIRISDR` - Feature support

### Unit Test Style
```cpp
class SquelchTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }
    void TearDown(void) { TestBaseClass::TearDown(); }
};

TEST_F(SquelchTest, normal_operation) {
    Squelch squelch;
    EXPECT_EQ(squelch.open_count(), 0);
}
```
- Test files: `test_<module>.cpp`
- Fixtures inherit from `TestBaseClass`
- Prefer `EXPECT_*` for non-fatal, `ASSERT_*` when continuation impossible

## Project Structure

```
RTLSDR-Airband/
├── CMakeLists.txt          # Root CMake config
├── src/
│   ├── CMakeLists.txt      # Source CMake config
│   ├── rtl_airband.cpp/h   # Main entry point
│   ├── config.cpp          # Configuration parsing
│   ├── squelch.cpp/h       # Squelch implementation
│   ├── filters.cpp/h       # Audio filters
│   ├── ctcss.cpp/h         # CTCSS tone detection
│   ├── input-*.cpp/h       # SDR input implementations
│   ├── output.cpp          # Output handling
│   ├── test_*.cpp          # Unit tests
│   └── CMakeModules/       # CMake find modules
├── scripts/
│   ├── reformat_code       # Code formatting
│   └── find_version        # Version detection
└── .github/workflows/      # CI workflows
```

## CI/CD

GitHub Actions:
- `ci_build.yml` - Build + test on Ubuntu/macOS ARM/x86
- `code_formatting.yml` - Clang-format check

## Dependencies

Required: libconfig++, LAME, libshout, libcurl, fftw3f, pthread, dl, m

Optional: librtlsdr, SoapySDR, libmirisdr, libpulse
