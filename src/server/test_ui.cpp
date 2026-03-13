#include "server/test_ui.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace zks::server {
namespace {

std::string escape_script_json(std::string json) {
    size_t pos = 0;
    while ((pos = json.find("</", pos)) != std::string::npos) {
        json.replace(pos, 2, "<\\/");
        pos += 3;
    }
    return json;
}

constexpr std::string_view kHtmlPrefix = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>zoo-keeper-server test console</title>
  <style>
    :root {
      --bg: #f6f0de;
      --panel: rgba(255, 251, 242, 0.88);
      --panel-strong: rgba(255, 248, 236, 0.96);
      --ink: #1f2a1f;
      --muted: #5b6854;
      --accent: #255f38;
      --accent-strong: #173d24;
      --accent-soft: rgba(37, 95, 56, 0.12);
      --warn: #7d2e1f;
      --warn-soft: rgba(125, 46, 31, 0.12);
      --border: rgba(31, 42, 31, 0.14);
      --shadow: 0 24px 80px rgba(41, 47, 33, 0.12);
      --radius: 20px;
      --mono: "SFMono-Regular", "SF Mono", "Liberation Mono", monospace;
      --serif: "Iowan Old Style", "Palatino Linotype", "Book Antiqua", serif;
    }

    * {
      box-sizing: border-box;
    }

    body {
      margin: 0;
      min-height: 100vh;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(90, 120, 63, 0.16), transparent 34%),
        linear-gradient(145deg, #f3ead2 0%, #f7f2e5 44%, #ece6d4 100%);
      font-family: var(--serif);
    }

    body::before {
      content: "";
      position: fixed;
      inset: 0;
      pointer-events: none;
      background:
        linear-gradient(rgba(255, 255, 255, 0.12), rgba(255, 255, 255, 0.12)),
        repeating-linear-gradient(
          135deg,
          rgba(26, 50, 28, 0.03) 0,
          rgba(26, 50, 28, 0.03) 3px,
          transparent 3px,
          transparent 17px
        );
    }

    main {
      position: relative;
      width: min(1180px, calc(100vw - 32px));
      margin: 24px auto 40px;
    }

    .hero,
    .panel {
      backdrop-filter: blur(14px);
      background: var(--panel);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
    }

    .hero {
      padding: 24px 24px 20px;
      overflow: hidden;
      position: relative;
    }

    .hero::after {
      content: "";
      position: absolute;
      right: -60px;
      top: -80px;
      width: 220px;
      height: 220px;
      border-radius: 50%;
      background: radial-gradient(circle, rgba(37, 95, 56, 0.22), transparent 68%);
    }

    .eyebrow {
      display: inline-flex;
      gap: 10px;
      align-items: center;
      padding: 6px 12px;
      border-radius: 999px;
      font: 700 12px/1.2 var(--mono);
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--accent-strong);
      background: rgba(255, 255, 255, 0.66);
      border: 1px solid rgba(37, 95, 56, 0.15);
    }

    h1 {
      margin: 14px 0 12px;
      max-width: 760px;
      font-size: clamp(2.2rem, 5vw, 4rem);
      line-height: 0.96;
      letter-spacing: -0.04em;
    }

    .hero p,
    .hint,
    .status-text,
    label,
    button,
    input,
    select,
    textarea {
      font-family: var(--mono);
    }

    .hero p {
      margin: 0;
      max-width: 760px;
      color: var(--muted);
      line-height: 1.6;
    }

    .route-list {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin: 18px 0 0;
      padding: 0;
      list-style: none;
    }

    .route-list code,
    .pill code,
    .message-content,
    pre {
      font-family: var(--mono);
    }

    .route-list li,
    .pill {
      border-radius: 999px;
      border: 1px solid rgba(31, 42, 31, 0.12);
      background: rgba(255, 255, 255, 0.72);
      padding: 8px 12px;
      color: var(--accent-strong);
      font-size: 0.92rem;
    }

    .layout {
      display: grid;
      grid-template-columns: minmax(0, 1.35fr) minmax(320px, 0.9fr);
      gap: 18px;
      margin-top: 18px;
    }

    .stack {
      display: grid;
      gap: 18px;
    }

    .panel {
      padding: 18px;
    }

    .panel h2,
    .panel h3 {
      margin: 0 0 12px;
      font-family: var(--mono);
      font-size: 0.95rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
    }

    .summary-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 12px;
    }

    .metric {
      padding: 14px;
      border-radius: 16px;
      background: var(--panel-strong);
      border: 1px solid rgba(31, 42, 31, 0.08);
    }

    .metric-label {
      display: block;
      font: 700 11px/1.2 var(--mono);
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--muted);
    }

    .metric-value {
      display: block;
      margin-top: 8px;
      font-size: 1.35rem;
      line-height: 1.1;
    }

    .row,
    .row-wrap,
    .inline-actions {
      display: flex;
      gap: 10px;
      align-items: center;
    }

    .row-wrap {
      flex-wrap: wrap;
    }

    .controls-grid {
      display: grid;
      gap: 12px;
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    .field,
    .field-wide {
      display: grid;
      gap: 8px;
    }

    .field-wide {
      grid-column: 1 / -1;
    }

    label {
      color: var(--muted);
      font-size: 0.83rem;
      letter-spacing: 0.04em;
      text-transform: uppercase;
    }

    input,
    select,
    textarea,
    button {
      width: 100%;
      border: 1px solid rgba(31, 42, 31, 0.14);
      border-radius: 14px;
      padding: 12px 14px;
      font-size: 0.95rem;
      color: var(--ink);
      background: rgba(255, 255, 255, 0.9);
    }

    textarea {
      min-height: 148px;
      resize: vertical;
      line-height: 1.5;
    }

    input:focus,
    select:focus,
    textarea:focus,
    button:focus {
      outline: 2px solid rgba(37, 95, 56, 0.22);
      outline-offset: 1px;
    }

    button {
      cursor: pointer;
      font-weight: 700;
      transition:
        transform 120ms ease,
        border-color 120ms ease,
        background 120ms ease;
    }

    button:hover:not(:disabled) {
      transform: translateY(-1px);
      border-color: rgba(23, 61, 36, 0.24);
    }

    button:disabled {
      cursor: not-allowed;
      opacity: 0.6;
    }

    .primary {
      color: #f7f4ea;
      background: linear-gradient(135deg, var(--accent), var(--accent-strong));
    }

    .secondary {
      background: rgba(255, 255, 255, 0.8);
    }

    .danger {
      background: linear-gradient(135deg, #a24530, #7d2e1f);
      color: #fff4ee;
    }

    .status-banner {
      margin-top: 14px;
      padding: 12px 14px;
      border-radius: 16px;
      background: var(--accent-soft);
      color: var(--accent-strong);
      border: 1px solid rgba(37, 95, 56, 0.12);
    }

    .status-banner.error {
      background: var(--warn-soft);
      color: var(--warn);
      border-color: rgba(125, 46, 31, 0.14);
    }

    .transcript {
      display: grid;
      gap: 12px;
      min-height: 200px;
      max-height: 520px;
      overflow: auto;
      padding-right: 4px;
    }

    .message {
      padding: 14px;
      border-radius: 16px;
      background: rgba(255, 255, 255, 0.72);
      border: 1px solid rgba(31, 42, 31, 0.08);
    }

    .message.user {
      background: rgba(37, 95, 56, 0.12);
      border-color: rgba(37, 95, 56, 0.16);
    }

    .message.assistant {
      background: rgba(255, 255, 255, 0.86);
    }

    .message.error {
      background: rgba(125, 46, 31, 0.08);
      border-color: rgba(125, 46, 31, 0.12);
    }

    .message-role {
      margin-bottom: 8px;
      color: var(--muted);
      font: 700 12px/1.2 var(--mono);
      letter-spacing: 0.1em;
      text-transform: uppercase;
    }

    .message-content {
      white-space: pre-wrap;
      line-height: 1.6;
    }

    .empty-state {
      padding: 18px;
      border-radius: 18px;
      background: rgba(255, 255, 255, 0.54);
      border: 1px dashed rgba(31, 42, 31, 0.18);
      color: var(--muted);
      line-height: 1.7;
    }

    .checkbox {
      display: inline-flex;
      width: auto;
      gap: 10px;
      align-items: center;
      padding: 0;
      color: var(--ink);
      border: 0;
      text-transform: none;
      letter-spacing: 0;
      font-size: 0.95rem;
    }

    .checkbox input {
      width: 18px;
      height: 18px;
      margin: 0;
      padding: 0;
    }

    .hint,
    .status-text {
      color: var(--muted);
      line-height: 1.6;
      font-size: 0.88rem;
    }

    .pill-row {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }

    pre {
      margin: 0;
      min-height: 180px;
      max-height: 320px;
      overflow: auto;
      padding: 14px;
      border-radius: 14px;
      background: #172014;
      color: #ebf1dd;
      font-size: 0.86rem;
      line-height: 1.55;
    }

    @media (max-width: 980px) {
      .layout {
        grid-template-columns: 1fr;
      }

      .summary-grid,
      .controls-grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <div class="eyebrow">Test build only <span>GET /_test</span></div>
      <h1>Local model console for exercising the server.</h1>
      <p>
        This page talks to the existing server endpoints from the same origin so you can check
        readiness, inspect the configured model, create or delete a session, and run stateless or
        sessioned chats against <code>/v1/chat/completions</code>.
      </p>
      <ul class="route-list">
        <li><code>/healthz</code></li>
        <li><code>/v1/models</code></li>
        <li><code>/v1/tools</code></li>
        <li><code>/v1/sessions</code></li>
        <li><code>/v1/chat/completions</code></li>
      </ul>
    </section>

    <div class="layout">
      <div class="stack">
        <section class="panel">
          <h2>Server Snapshot</h2>
          <div class="summary-grid">
            <div class="metric">
              <span class="metric-label">Health</span>
              <span class="metric-value" id="health-value">Loading</span>
            </div>
            <div class="metric">
              <span class="metric-label">Model</span>
              <span class="metric-value" id="model-value">-</span>
            </div>
            <div class="metric">
              <span class="metric-label">Sessions</span>
              <span class="metric-value" id="session-value">-</span>
            </div>
          </div>
          <div class="row-wrap" style="margin-top: 14px;">
            <button class="secondary" id="refresh-button" type="button">Refresh server data</button>
            <div class="pill-row" id="tool-pills"></div>
          </div>
          <div class="status-banner" id="status-banner">
            <span class="status-text" id="status-text">Loading server metadata.</span>
          </div>
        </section>

        <section class="panel">
          <h2>Conversation</h2>
          <div class="transcript" id="transcript"></div>
        </section>

        <section class="panel">
          <h2>Prompt</h2>
          <div class="controls-grid">
            <div class="field">
              <label for="model-select">Model</label>
              <select id="model-select"></select>
            </div>
            <div class="field">
              <label for="session-id">Active session</label>
              <input id="session-id" type="text" readonly value="No active session" />
            </div>
            <div class="field-wide">
              <label for="prompt-input">Message</label>
              <textarea
                id="prompt-input"
                placeholder="Write a message to send to /v1/chat/completions"
              ></textarea>
            </div>
            <div class="field-wide">
              <label class="checkbox" for="stream-checkbox">
                <input id="stream-checkbox" type="checkbox" />
                Stream the response over SSE
              </label>
            </div>
            <div class="field-wide">
              <label class="checkbox" for="session-checkbox">
                <input id="session-checkbox" type="checkbox" />
                Send this turn with the active server session
              </label>
              <div class="hint">
                Session mode sends exactly one new user message plus <code>session_id</code>.
                Stateless mode resends the full local transcript.
              </div>
            </div>
          </div>
          <div class="inline-actions" style="margin-top: 14px;">
            <button class="primary" id="send-button" type="button">Send message</button>
            <button class="secondary" id="stop-button" type="button" disabled>Stop stream</button>
            <button class="secondary" id="clear-button" type="button">Clear local transcript</button>
          </div>
        </section>
      </div>

      <div class="stack">
        <section class="panel">
          <h2>Session Controls</h2>
          <div class="field">
            <label for="session-prompt">Optional system prompt</label>
            <textarea
              id="session-prompt"
              placeholder="Optional session-specific system prompt for POST /v1/sessions"
            ></textarea>
          </div>
          <div class="inline-actions" style="margin-top: 14px;">
            <button class="primary" id="create-session-button" type="button">Create session</button>
            <button class="danger" id="delete-session-button" type="button">Delete session</button>
          </div>
          <p class="hint" style="margin: 12px 0 0;">
            Deleting the session clears the server-owned conversation context. Reloading this page
            only clears the local browser state.
          </p>
        </section>

        <section class="panel">
          <h2>Last Request</h2>
          <pre id="request-payload">// No request sent yet.</pre>
        </section>

        <section class="panel">
          <h2>Last Response</h2>
          <pre id="response-payload">// No response yet.</pre>
        </section>
      </div>
    </div>
  </main>

  <script id="zks-test-ui-boot" type="application/json">
)HTML";

constexpr std::string_view kHtmlSuffix = R"HTML(
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
)HTML";

} // namespace

drogon::HttpResponsePtr make_test_ui_response(const HealthSnapshot& snapshot) {
    const auto boot_payload = escape_script_json(
        nlohmann::json{{"health",
                        {{"ready", snapshot.ready},
                         {"model", {{"id", snapshot.model_id}}},
                         {"sessions",
                          {{"enabled", snapshot.sessions.enabled},
                           {"active", snapshot.sessions.active},
                           {"max_sessions", snapshot.sessions.max_sessions},
                           {"idle_ttl_seconds", snapshot.sessions.idle_ttl_seconds}}}}}}
            .dump());

    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeCodeAndCustomString(drogon::CT_TEXT_HTML, "text/html; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::string(kHtmlPrefix) + boot_payload + std::string(kHtmlSuffix));
    return response;
}

void register_test_ui_routes(const std::shared_ptr<const ServerRuntime>& runtime) {
    std::weak_ptr<const ServerRuntime> weak_runtime = runtime;
    drogon::app().registerHandler(
        "/_test",
        [weak_runtime](const drogon::HttpRequestPtr&,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto runtime = weak_runtime.lock();
            if (!runtime) {
                callback(make_test_ui_response({false, "", {}}));
                return;
            }

            callback(make_test_ui_response(runtime->health_snapshot()));
        },
        {drogon::Get});
}

} // namespace zks::server
