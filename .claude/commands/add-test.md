Add a new test to the project. Follow these steps:

1. **Create test file** at `tests/<name>_test.cpp`
   ```cpp
   #include <gtest/gtest.h>
   // Include the header(s) for the module under test

   TEST(<TestSuite>, <TestName>) {
       // ...
   }
   ```

2. **Use FakeChatService** if the test needs a `ChatService` instance
   - `#include "fake_chat_service.hpp"`
   - Set mode: `fake.set_mode(FakeCompletionMode::Success)`
   - Set response: `fake.set_response(CompletionResult{...})`
   - See `tests/fake_chat_service.hpp` for all available modes

3. **Register** in `tests/CMakeLists.txt`:
   ```cmake
   zks_add_test(<name>_test SOURCES <name>_test.cpp)
   ```
   Add `OWN_MAIN` if you need a custom `main()`. Add `LIBS <target>` for extra link deps.

4. **Run the test**:
   ```bash
   scripts/test -R <name>_test
   ```

5. **Check formatting**:
   ```bash
   scripts/format-check
   ```
