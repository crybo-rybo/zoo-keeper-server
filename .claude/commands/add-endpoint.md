Add a new HTTP endpoint to the server. Follow these steps in order:

1. **Add route handler** in `src/server/api_routes.cpp`
   - Copy the lambda registration pattern from `register_health_routes` or an existing route
   - Capture `weak_runtime` and lock it at the start of the handler
   - Wrap the response callback with `with_metrics()`
   - If the endpoint is complex (like chat completions), create a separate file like `completion_controller.cpp`

2. **Add JSON parse/serialize** in `src/server/api_json.cpp` and `src/server/api_json.hpp`
   - Add `parse_<request>()` function returning `ApiResult<RequestType>`
   - Add `make_<response>()` function returning `drogon::HttpResponsePtr`
   - Use `reject_unknown_keys()` on all parsed JSON objects

3. **Add types** to `src/server/api_types.hpp` if new request/response structs are needed
   - Use `ApiResult<T>` for fallible operations
   - Use the existing error helpers (`invalid_request_error`, etc.)

4. **Add OpenAPI entry** in `docs/openapi.yaml`
   - Document request body, response schema, and error responses

5. **Add test** in `tests/<name>_test.cpp`
   - Use `FakeChatService` if the endpoint interacts with `ChatService`
   - See `tests/fake_chat_service.hpp` for available modes
   - Register in `tests/CMakeLists.txt` with `zks_add_test()`

6. **Verify**
   ```bash
   scripts/test
   scripts/format-check
   ```
