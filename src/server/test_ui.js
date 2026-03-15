
  </script>
  <script>
    (() => {
      const boot = JSON.parse(document.getElementById("zks-test-ui-boot").textContent);
      const state = {
        health: boot.health,
        models: boot.health.model.id ? [{ id: boot.health.model.id }] : [],
        tools: [],
        transcript: [],
        sessionId: null,
        requestText: "// No request sent yet.",
        responseText: "// No response yet.",
        activeController: null
      };

      const ui = {
        healthValue: document.getElementById("health-value"),
        modelValue: document.getElementById("model-value"),
        sessionValue: document.getElementById("session-value"),
        toolPills: document.getElementById("tool-pills"),
        statusBanner: document.getElementById("status-banner"),
        statusText: document.getElementById("status-text"),
        refreshButton: document.getElementById("refresh-button"),
        transcript: document.getElementById("transcript"),
        modelSelect: document.getElementById("model-select"),
        sessionId: document.getElementById("session-id"),
        promptInput: document.getElementById("prompt-input"),
        streamCheckbox: document.getElementById("stream-checkbox"),
        sessionCheckbox: document.getElementById("session-checkbox"),
        sendButton: document.getElementById("send-button"),
        stopButton: document.getElementById("stop-button"),
        clearButton: document.getElementById("clear-button"),
        sessionPrompt: document.getElementById("session-prompt"),
        createSessionButton: document.getElementById("create-session-button"),
        deleteSessionButton: document.getElementById("delete-session-button"),
        requestPayload: document.getElementById("request-payload"),
        responsePayload: document.getElementById("response-payload")
      };

      function setStatus(message, isError = false) {
        ui.statusText.textContent = message;
        ui.statusBanner.classList.toggle("error", isError);
      }

      function formatJson(value) {
        return JSON.stringify(value, null, 2);
      }

      function escapeError(status, payload) {
        if (payload && payload.error && payload.error.message) {
          return new Error(payload.error.message);
        }
        return new Error("Request failed with status " + status);
      }

      async function readJsonResponse(response) {
        const text = await response.text();
        if (!text) {
          return null;
        }
        return JSON.parse(text);
      }

      async function fetchJson(url, options = {}) {
        const response = await fetch(url, options);
        const payload = await readJsonResponse(response);
        if (!response.ok) {
          throw escapeError(response.status, payload);
        }
        return payload;
      }

      function syncModelSelect() {
        const current = ui.modelSelect.value;
        const nextModelId =
          current || (state.models[0] ? state.models[0].id : state.health.model.id || "");
        ui.modelSelect.textContent = "";
        for (const model of state.models) {
          const option = document.createElement("option");
          option.value = model.id;
          option.textContent = model.id;
          ui.modelSelect.append(option);
        }
        if (!state.models.length && state.health.model.id) {
          const option = document.createElement("option");
          option.value = state.health.model.id;
          option.textContent = state.health.model.id;
          ui.modelSelect.append(option);
        }
        if (nextModelId) {
          ui.modelSelect.value = nextModelId;
        }
      }

      function selectedModel() {
        return ui.modelSelect.value || state.health.model.id || "";
      }

      function syncSessionControls() {
        const sessionHealth = state.health.sessions || {};
        const sessionsEnabled = Boolean(sessionHealth.enabled);
        ui.sessionCheckbox.disabled = !sessionsEnabled;
        ui.createSessionButton.disabled = !sessionsEnabled || Boolean(state.activeController);
        ui.deleteSessionButton.disabled = !state.sessionId || Boolean(state.activeController);
        if (!sessionsEnabled) {
          ui.sessionCheckbox.checked = false;
        }
        ui.sessionId.value = state.sessionId || "No active session";
      }

      function renderSummary() {
        const sessionHealth = state.health.sessions || {};
        ui.healthValue.textContent = state.health.ready ? "Ready" : "Starting";
        ui.modelValue.textContent = state.health.model.id || "-";
        ui.sessionValue.textContent = sessionHealth.enabled
          ? sessionHealth.active + " / " + sessionHealth.max_sessions
          : "Disabled";
      }

      function renderTools() {
        ui.toolPills.textContent = "";
        if (!state.tools.length) {
          const pill = document.createElement("div");
          pill.className = "pill";
          pill.textContent = "No tools advertised";
          ui.toolPills.append(pill);
          return;
        }

        for (const tool of state.tools) {
          const pill = document.createElement("div");
          pill.className = "pill";
          pill.innerHTML = "<code>" + tool.function.name + "</code>";
          ui.toolPills.append(pill);
        }
      }

      function renderTranscript() {
        ui.transcript.textContent = "";
        if (!state.transcript.length) {
          const empty = document.createElement("div");
          empty.className = "empty-state";
          empty.textContent =
            "No turns yet. Send a stateless request or create a session to begin.";
          ui.transcript.append(empty);
          return;
        }

        for (const message of state.transcript) {
          const item = document.createElement("article");
          item.className = "message " + message.role;
          const role = document.createElement("div");
          role.className = "message-role";
          role.textContent = message.role;
          const content = document.createElement("div");
          content.className = "message-content";
          content.textContent = message.content;
          item.append(role, content);
          ui.transcript.append(item);
        }
        ui.transcript.scrollTop = ui.transcript.scrollHeight;
      }

      function renderDebugPanels() {
        ui.requestPayload.textContent = state.requestText;
        ui.responsePayload.textContent = state.responseText;
      }

      function renderAll() {
        syncModelSelect();
        syncSessionControls();
        renderSummary();
        renderTools();
        renderTranscript();
        renderDebugPanels();
      }

      async function refreshMetadata() {
        setStatus("Refreshing /healthz, /v1/models, and /v1/tools.");
        const [health, models, tools] = await Promise.all([
          fetchJson("/healthz"),
          fetchJson("/v1/models"),
          fetchJson("/v1/tools")
        ]);

        state.health = health;
        state.models = Array.isArray(models && models.data) ? models.data : [];
        state.tools = Array.isArray(tools && tools.data) ? tools.data : [];
        renderAll();
        setStatus("Server metadata refreshed.");
      }

      function buildStatelessMessages(prompt) {
        return state.transcript.concat([{ role: "user", content: prompt }]).map((message) => {
          const payload = { role: message.role, content: message.content };
          if (message.tool_call_id) {
            payload.tool_call_id = message.tool_call_id;
          }
          return payload;
        });
      }

      function buildChatRequest(prompt) {
        const model = selectedModel();
        if (!model) {
          throw new Error("No model is available yet.");
        }

        const request = {
          model,
          stream: ui.streamCheckbox.checked
        };

        if (ui.sessionCheckbox.checked) {
          if (!state.sessionId) {
            throw new Error("Create a session before enabling session chat.");
          }
          request.session_id = state.sessionId;
          request.messages = [{ role: "user", content: prompt }];
          return request;
        }

        request.messages = buildStatelessMessages(prompt);
        return request;
      }

      function appendUserTurn(prompt) {
        state.transcript.push({ role: "user", content: prompt });
      }

      function appendAssistantTurn(content) {
        state.transcript.push({ role: "assistant", content });
      }

      function beginRequest(request) {
        state.requestText = formatJson(request);
        state.responseText = "// Waiting for response...";
        renderDebugPanels();
        ui.sendButton.disabled = true;
        ui.stopButton.disabled = !request.stream;
        ui.createSessionButton.disabled = true;
        ui.deleteSessionButton.disabled = true;
      }

      function finishRequest() {
        state.activeController = null;
        ui.sendButton.disabled = false;
        ui.stopButton.disabled = true;
        syncSessionControls();
      }

      async function runNonStreamingCompletion(request) {
        const response = await fetchJson("/v1/chat/completions", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify(request)
        });
        state.responseText = formatJson(response);
        appendAssistantTurn(
          (((response || {}).choices || [])[0] || {}).message
            ? response.choices[0].message.content || ""
            : ""
        );
      }

      function parseEventBlock(block) {
        const lines = block.split("\n");
        const dataLines = [];
        for (const line of lines) {
          if (line.startsWith("data:")) {
            dataLines.push(line.slice(5).trimStart());
          }
        }
        return dataLines.join("\n");
      }

      async function runStreamingCompletion(request) {
        state.activeController = new AbortController();
        const rawFrames = [];

        const response = await fetch("/v1/chat/completions", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify(request),
          signal: state.activeController.signal
        });

        if (!response.ok) {
          const payload = await readJsonResponse(response);
          throw escapeError(response.status, payload);
        }

        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        const assistant = { role: "assistant", content: "" };
        state.transcript.push(assistant);
        renderTranscript();

        let buffer = "";
        while (true) {
          const { value, done } = await reader.read();
          buffer += decoder.decode(value || new Uint8Array(), { stream: !done });

          let boundary = buffer.indexOf("\n\n");
          while (boundary !== -1) {
            const block = buffer.slice(0, boundary);
            buffer = buffer.slice(boundary + 2);
            const data = parseEventBlock(block);
            if (data) {
              rawFrames.push("data: " + data);
              if (data !== "[DONE]") {
                const payload = JSON.parse(data);
                const choice = (((payload || {}).choices || [])[0]) || {};
                const delta = choice.delta || {};
                if (typeof delta.content === "string") {
                  assistant.content += delta.content;
                  renderTranscript();
                }
              }
            }
            boundary = buffer.indexOf("\n\n");
          }

          state.responseText = rawFrames.join("\n\n");
          renderDebugPanels();

          if (done) {
            break;
          }
        }
      }

      async function sendPrompt() {
        const prompt = ui.promptInput.value.trim();
        if (!prompt) {
          setStatus("Enter a prompt before sending.", true);
          return;
        }

        try {
          const request = buildChatRequest(prompt);
          appendUserTurn(prompt);
          ui.promptInput.value = "";
          renderTranscript();
          beginRequest(request);
          setStatus(
            request.stream
              ? "Streaming /v1/chat/completions."
              : "Sending /v1/chat/completions."
          );

          if (request.stream) {
            await runStreamingCompletion(request);
          } else {
            await runNonStreamingCompletion(request);
          }

          setStatus("Request finished successfully.");
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          state.responseText = message;
          state.transcript.push({ role: "error", content: message });
          renderAll();
          setStatus(message, true);
        } finally {
          finishRequest();
        }
      }

      async function createSession() {
        if (state.sessionId) {
          setStatus("Delete the current session before creating a new one.", true);
          return;
        }

        try {
          const payload = { model: selectedModel() };
          const systemPrompt = ui.sessionPrompt.value.trim();
          if (systemPrompt) {
            payload.system_prompt = systemPrompt;
          }

          state.requestText = formatJson(payload);
          renderDebugPanels();

          const response = await fetchJson("/v1/sessions", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify(payload)
          });

          state.responseText = formatJson(response);
          state.sessionId = response.id;
          ui.sessionCheckbox.checked = true;
          state.transcript = [];
          renderAll();
          setStatus("Created session " + response.id + ".");
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          state.responseText = message;
          renderDebugPanels();
          setStatus(message, true);
        }
      }

      async function deleteSession() {
        if (!state.sessionId) {
          setStatus("There is no active session to delete.", true);
          return;
        }

        try {
          state.requestText = "DELETE /v1/sessions/" + encodeURIComponent(state.sessionId);
          renderDebugPanels();

          await fetchJson("/v1/sessions/" + encodeURIComponent(state.sessionId), {
            method: "DELETE"
          });

          state.responseText = "204 No Content";
          state.sessionId = null;
          ui.sessionCheckbox.checked = false;
          renderAll();
          setStatus("Deleted the active session.");
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          state.responseText = message;
          renderDebugPanels();
          setStatus(message, true);
        }
      }

      function clearTranscript() {
        state.transcript = [];
        renderTranscript();
        setStatus("Cleared the local transcript.");
      }

      function stopStreaming() {
        if (!state.activeController) {
          return;
        }
        state.activeController.abort();
        setStatus("Streaming request aborted from the browser.", true);
      }

      ui.refreshButton.addEventListener("click", async () => {
        try {
          await refreshMetadata();
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          setStatus(message, true);
        }
      });

      ui.sendButton.addEventListener("click", sendPrompt);
      ui.stopButton.addEventListener("click", stopStreaming);
      ui.clearButton.addEventListener("click", clearTranscript);
      ui.createSessionButton.addEventListener("click", createSession);
      ui.deleteSessionButton.addEventListener("click", deleteSession);
      ui.promptInput.addEventListener("keydown", (event) => {
        if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
          event.preventDefault();
          sendPrompt();
        }
      });

      renderAll();
      refreshMetadata().catch((error) => {
        const message = error instanceof Error ? error.message : String(error);
        setStatus(message, true);
      });
    })();
  </script>
</body>
</html>
