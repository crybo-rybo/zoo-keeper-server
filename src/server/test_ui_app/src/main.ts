import "./styles.css";

type ChapterId = "overview" | "inference" | "memory" | "tools" | "operations";
type StatusTone = "neutral" | "success" | "error";
type TranscriptRole = "user" | "assistant" | "error";

interface HealthPayload {
  ready: boolean;
  version?: string;
  model: { id: string };
  sessions: {
    enabled: boolean;
    active: number;
    max_sessions: number;
    idle_ttl_seconds: number;
  };
}

interface BootPayload {
  health: HealthPayload;
}

interface ModelEntry {
  id: string;
  object?: string;
  owned_by?: string;
}

interface ModelsResponse {
  data?: ModelEntry[];
}

interface ToolDefinition {
  type: string;
  function: {
    name: string;
    description: string;
    parameters?: Record<string, unknown>;
  };
}

interface ToolsResponse {
  data?: ToolDefinition[];
}

interface SessionSummary {
  id: string;
  model: string;
  created: number;
  last_used: number;
  expires_at: number;
}

interface MetricsResponse {
  requests_total: number;
  requests_errors: number;
  requests_cancelled_total: number;
  requests_queue_rejected_total: number;
  stream_disconnects_total: number;
  tool_invocations_total: number;
  tool_failures_total: number;
  tool_timeouts_total: number;
  active_sessions: number;
  model_id: string;
  uptime_seconds: number;
}

interface ToolInvocation {
  id: string;
  name: string;
  status: string;
  arguments?: unknown;
  result?: unknown;
  error?: string;
}

interface ChatCompletionResponse {
  choices?: Array<{
    message?: {
      role?: string;
      content?: string;
    };
  }>;
  tool_invocations?: ToolInvocation[];
}

interface TranscriptEntry {
  role: TranscriptRole;
  content: string;
}

interface PendingStreamResult {
  content: string;
  rawFrames: string;
}

const chapterIds: ChapterId[] = ["overview", "inference", "memory", "tools", "operations"];

function getElement<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Missing required element: ${id}`);
  }
  return element as T;
}

const boot = JSON.parse(getElement<HTMLScriptElement>("zks-test-ui-boot").textContent ?? "{}") as BootPayload;

const state = {
  currentChapter: "overview" as ChapterId,
  health: boot.health,
  models: boot.health.model.id ? [{ id: boot.health.model.id }] : ([] as ModelEntry[]),
  tools: [] as ToolDefinition[],
  metrics: null as MetricsResponse | null,
  inferenceTranscript: [] as TranscriptEntry[],
  memoryTranscript: [] as TranscriptEntry[],
  memorySession: null as SessionSummary | null,
  toolsResponse: "// No tool demo run yet.",
  toolInvocations: [] as ToolInvocation[],
  lastRequestText: "// No request yet.",
  lastResponseText: "// No response yet.",
  lastMemorySessionError: null as string | null,
  busy: false,
  activeController: null as AbortController | null
};

const ui = {
  body: document.body,
  progressBarValue: getElement<HTMLDivElement>("progress-bar-value"),
  statusChip: getElement<HTMLDivElement>("status-chip"),
  statusText: getElement<HTMLSpanElement>("status-text"),
  refreshAllButton: getElement<HTMLButtonElement>("refresh-all-button"),
  heroReadyValue: getElement<HTMLSpanElement>("hero-ready-value"),
  heroModelValue: getElement<HTMLSpanElement>("hero-model-value"),
  heroVersionValue: getElement<HTMLSpanElement>("hero-version-value"),
  heroSessionValue: getElement<HTMLSpanElement>("hero-session-value"),
  heroLede: getElement<HTMLParagraphElement>("hero-lede"),
  modelSelect: getElement<HTMLSelectElement>("model-select"),
  inferencePrompt: getElement<HTMLTextAreaElement>("inference-prompt"),
  inferenceStream: getElement<HTMLInputElement>("inference-stream"),
  inferenceSend: getElement<HTMLButtonElement>("inference-send"),
  inferenceStop: getElement<HTMLButtonElement>("inference-stop"),
  inferenceClear: getElement<HTMLButtonElement>("inference-clear"),
  inferenceTranscript: getElement<HTMLDivElement>("inference-transcript"),
  inferenceMeta: getElement<HTMLSpanElement>("inference-meta"),
  memoryDisabledState: getElement<HTMLDivElement>("memory-disabled-state"),
  memoryEnabledShell: getElement<HTMLDivElement>("memory-enabled-shell"),
  memorySystemPrompt: getElement<HTMLTextAreaElement>("memory-system-prompt"),
  memoryCreateSession: getElement<HTMLButtonElement>("memory-create-session"),
  memoryRefreshSession: getElement<HTMLButtonElement>("memory-refresh-session"),
  memoryDeleteSession: getElement<HTMLButtonElement>("memory-delete-session"),
  memorySessionId: getElement<HTMLElement>("memory-session-id"),
  memorySessionSummary: getElement<HTMLElement>("memory-session-summary"),
  memoryPrompt: getElement<HTMLTextAreaElement>("memory-prompt"),
  memorySend: getElement<HTMLButtonElement>("memory-send"),
  memoryTranscript: getElement<HTMLDivElement>("memory-transcript"),
  memorySessionJson: getElement<HTMLPreElement>("memory-session-json"),
  memoryMetaDetail: getElement<HTMLSpanElement>("memory-meta-detail"),
  toolsEmptyState: getElement<HTMLDivElement>("tools-empty-state"),
  toolsEnabledShell: getElement<HTMLDivElement>("tools-enabled-shell"),
  toolsCatalog: getElement<HTMLDivElement>("tools-catalog"),
  toolsPrompt: getElement<HTMLTextAreaElement>("tools-prompt"),
  toolsSend: getElement<HTMLButtonElement>("tools-send"),
  toolsResponse: getElement<HTMLDivElement>("tools-response"),
  toolsTrace: getElement<HTMLDivElement>("tools-trace"),
  toolsTraceMeta: getElement<HTMLSpanElement>("tools-trace-meta"),
  operationsRefreshMetrics: getElement<HTMLButtonElement>("operations-refresh-metrics"),
  metricRequestsTotal: getElement<HTMLElement>("metric-requests-total"),
  metricRequestsErrors: getElement<HTMLElement>("metric-requests-errors"),
  metricRequestsCancelled: getElement<HTMLElement>("metric-requests-cancelled"),
  metricRequestsRejected: getElement<HTMLElement>("metric-requests-rejected"),
  metricToolInvocations: getElement<HTMLElement>("metric-tool-invocations"),
  metricUptime: getElement<HTMLElement>("metric-uptime"),
  lastRequest: getElement<HTMLPreElement>("last-request"),
  lastResponse: getElement<HTMLPreElement>("last-response"),
  operationsRequest: getElement<HTMLPreElement>("operations-request"),
  operationsResponse: getElement<HTMLPreElement>("operations-response")
};

const siteHeader = getElement<HTMLElement>("site-header");
const navLinks = new Map<ChapterId, HTMLAnchorElement>();
const chapterSections = new Map<ChapterId, HTMLElement>();
for (const chapterId of chapterIds) {
  navLinks.set(
    chapterId,
    siteHeader.querySelector(`[data-nav-link="${chapterId}"]`) as HTMLAnchorElement
  );
  const section = document.querySelector<HTMLElement>(`[data-chapter="${chapterId}"]`);
  if (section) {
    chapterSections.set(chapterId, section);
  }
}

function setStatus(message: string, tone: StatusTone = "neutral"): void {
  ui.statusText.textContent = message;
  ui.statusChip.dataset.tone = tone;
}

function formatJson(value: unknown): string {
  if (typeof value === "string") {
    return value;
  }
  return JSON.stringify(value, null, 2);
}

function escapeHtml(text: string): string {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function contentFromResponse(response: ChatCompletionResponse | null): string {
  return response?.choices?.[0]?.message?.content ?? "";
}

function selectedModel(): string {
  return ui.modelSelect.value || state.health.model.id || "";
}

async function readJsonResponse(response: Response): Promise<unknown> {
  const text = await response.text();
  if (!text) {
    return null;
  }
  return JSON.parse(text) as unknown;
}

function toErrorMessage(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

function throwResponseError(status: number, payload: unknown): never {
  if (payload && typeof payload === "object" && "error" in payload) {
    const errorPayload = (payload as { error?: { message?: string } }).error;
    if (errorPayload?.message) {
      throw new Error(errorPayload.message);
    }
  }
  throw new Error(`Request failed with status ${status}`);
}

async function fetchJson<T>(url: string, options: RequestInit = {}): Promise<T> {
  const response = await fetch(url, options);
  const payload = await readJsonResponse(response);
  if (!response.ok) {
    throwResponseError(response.status, payload);
  }
  return payload as T;
}

function formatTimestamp(epochSeconds: number): string {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "short"
  }).format(new Date(epochSeconds * 1000));
}

function formatUptime(seconds: number): string {
  if (seconds < 60) {
    return `${seconds}s`;
  }
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) {
    return `${minutes}m`;
  }
  const hours = Math.floor(minutes / 60);
  const remainder = minutes % 60;
  return `${hours}h ${remainder}m`;
}

function buildHistoryMessages(entries: TranscriptEntry[]): Array<{ role: "user" | "assistant"; content: string }> {
  return entries
    .filter((entry): entry is TranscriptEntry & { role: "user" | "assistant" } => entry.role === "user" || entry.role === "assistant")
    .map((entry) => ({ role: entry.role, content: entry.content }));
}

function renderTranscript(container: HTMLDivElement, entries: TranscriptEntry[], emptyText: string): void {
  if (!entries.length) {
    container.innerHTML = `<p class="transcript__empty">${escapeHtml(emptyText)}</p>`;
    return;
  }

  container.innerHTML = entries
    .map((entry) => {
      return `
        <article class="bubble" data-role="${entry.role}">
          <span class="bubble__role">${entry.role}</span>
          <div class="bubble__content">${escapeHtml(entry.content)}</div>
        </article>
      `;
    })
    .join("");
  container.scrollTop = container.scrollHeight;
}

function summarizeToolSchema(tool: ToolDefinition): string {
  const parameters = tool.function.parameters;
  if (!parameters || typeof parameters !== "object") {
    return "No schema metadata";
  }
  const properties = (parameters as { properties?: Record<string, unknown> }).properties;
  if (!properties) {
    return "No parameter properties";
  }
  const names = Object.keys(properties);
  if (!names.length) {
    return "No parameters";
  }
  return names.join(", ");
}

function renderToolCatalog(): void {
  if (!state.tools.length) {
    ui.toolsCatalog.innerHTML = "";
    ui.toolsEmptyState.hidden = false;
    ui.toolsEnabledShell.hidden = true;
    ui.toolsTraceMeta.textContent = "No tool catalog";
    return;
  }

  ui.toolsEmptyState.hidden = true;
  ui.toolsEnabledShell.hidden = false;
  ui.toolsCatalog.innerHTML = state.tools
    .map((tool) => {
      return `
        <article class="catalog__item">
          <div>
            <p class="catalog__label">${escapeHtml(tool.function.name)}</p>
            <h3>${escapeHtml(tool.function.description)}</h3>
          </div>
          <p class="catalog__schema">${escapeHtml(summarizeToolSchema(tool))}</p>
        </article>
      `;
    })
    .join("");
}

function renderToolTrace(): void {
  if (!state.toolInvocations.length) {
    ui.toolsTrace.innerHTML = `<p class="trace-list__empty">No tool invocations recorded yet.</p>`;
    ui.toolsTraceMeta.textContent = state.tools.length ? "Awaiting invocation" : "No tool catalog";
    return;
  }

  ui.toolsTraceMeta.textContent = `${state.toolInvocations.length} invocation${state.toolInvocations.length === 1 ? "" : "s"}`;
  ui.toolsTrace.innerHTML = state.toolInvocations
    .map((invocation) => {
      const argumentText = invocation.arguments ? formatJson(invocation.arguments) : "{}";
      const resultText = invocation.result ? formatJson(invocation.result) : invocation.error ?? "No result";
      return `
        <article class="trace-item">
          <div class="trace-item__header">
            <strong>${escapeHtml(invocation.name)}</strong>
            <span>${escapeHtml(invocation.status)}</span>
          </div>
          <pre>${escapeHtml(argumentText)}</pre>
          <pre>${escapeHtml(resultText)}</pre>
        </article>
      `;
    })
    .join("");
}

function updateInspectors(): void {
  ui.lastRequest.textContent = state.lastRequestText;
  ui.lastResponse.textContent = state.lastResponseText;
  ui.operationsRequest.textContent = state.lastRequestText;
  ui.operationsResponse.textContent = state.lastResponseText;
}

function syncModelSelect(): void {
  const currentValue = ui.modelSelect.value;
  const nextValue = currentValue || state.models[0]?.id || state.health.model.id || "";
  ui.modelSelect.textContent = "";

  const entries = state.models.length ? state.models : [{ id: state.health.model.id }];
  for (const model of entries) {
    const option = document.createElement("option");
    option.value = model.id;
    option.textContent = model.id;
    ui.modelSelect.append(option);
  }

  if (nextValue) {
    ui.modelSelect.value = nextValue;
  }
}

function renderOverview(): void {
  ui.heroReadyValue.textContent = state.health.ready ? "Ready" : "Starting";
  ui.heroModelValue.textContent = state.health.model.id || "-";
  ui.heroVersionValue.textContent = state.health.version || "-";
  ui.heroSessionValue.textContent = state.health.sessions.enabled
    ? `${state.health.sessions.active} / ${state.health.sessions.max_sessions}`
    : "Disabled";
  ui.heroLede.textContent = state.health.ready
    ? "Explore the real capabilities currently exposed by this server, backed by the same APIs your clients use in production."
    : "The model is still warming up. The atlas stays available and will refresh live metadata as the server becomes ready.";
  ui.body.dataset.ready = String(state.health.ready);
}

function renderMemory(): void {
  const sessionsEnabled = state.health.sessions.enabled;
  ui.memoryDisabledState.hidden = sessionsEnabled;
  ui.memoryEnabledShell.hidden = !sessionsEnabled;
  ui.memoryDeleteSession.disabled = !state.memorySession || state.busy;

  if (!state.memorySession) {
    ui.memorySessionId.textContent = "No active session";
    ui.memorySessionSummary.textContent = sessionsEnabled
      ? "Create a session to begin the continuity demo."
      : "Sessions are disabled for this server.";
    ui.memorySessionJson.textContent = "// No session created yet.";
    ui.memoryMetaDetail.textContent = sessionsEnabled ? "Continuity path" : "Disabled";
  } else {
    ui.memorySessionId.textContent = state.memorySession.id;
    ui.memorySessionSummary.textContent = `Last used ${formatTimestamp(state.memorySession.last_used)} • expires ${formatTimestamp(state.memorySession.expires_at)}`;
    ui.memorySessionJson.textContent = formatJson(state.memorySession);
    ui.memoryMetaDetail.textContent = "Session is live";
  }

  renderTranscript(
    ui.memoryTranscript,
    state.memoryTranscript,
    "Session turns will appear here once you create a session and start the guided demo."
  );
}

function renderOperations(): void {
  const metrics = state.metrics;
  ui.metricRequestsTotal.textContent = String(metrics?.requests_total ?? 0);
  ui.metricRequestsErrors.textContent = String(metrics?.requests_errors ?? 0);
  ui.metricRequestsCancelled.textContent = String(metrics?.requests_cancelled_total ?? 0);
  ui.metricRequestsRejected.textContent = String(metrics?.requests_queue_rejected_total ?? 0);
  ui.metricToolInvocations.textContent = String(metrics?.tool_invocations_total ?? 0);
  ui.metricUptime.textContent = formatUptime(metrics?.uptime_seconds ?? 0);
}

function renderAll(): void {
  syncModelSelect();
  renderOverview();
  renderTranscript(
    ui.inferenceTranscript,
    state.inferenceTranscript,
    "No inference turns yet. Use a preset or send a custom prompt."
  );
  ui.inferenceMeta.textContent = ui.inferenceStream.checked ? "Streaming mode" : "Single response";
  renderMemory();
  renderToolCatalog();
  ui.toolsResponse.textContent = state.toolsResponse;
  renderToolTrace();
  renderOperations();
  updateInspectors();
  syncControls();
}

function syncControls(): void {
  ui.refreshAllButton.disabled = state.busy;
  ui.inferenceSend.disabled = state.busy;
  ui.inferenceClear.disabled = state.busy;
  ui.inferenceStop.disabled = !state.activeController;
  ui.memoryCreateSession.disabled = state.busy || !!state.memorySession || !state.health.sessions.enabled;
  ui.memoryRefreshSession.disabled = state.busy || !state.memorySession;
  ui.memoryDeleteSession.disabled = state.busy || !state.memorySession;
  ui.memorySend.disabled = state.busy || !state.health.sessions.enabled;
  ui.toolsSend.disabled = state.busy;
  ui.operationsRefreshMetrics.disabled = state.busy;
}

function beginActivity(request: unknown): void {
  state.busy = true;
  state.lastRequestText = formatJson(request);
  state.lastResponseText = "// Waiting for response...";
  updateInspectors();
  syncControls();
}

function finishActivity(): void {
  state.busy = false;
  state.activeController = null;
  syncControls();
}

function buildCompletionRequest(prompt: string, entries: TranscriptEntry[]): Record<string, unknown> {
  const model = selectedModel();
  if (!model) {
    throw new Error("No model is available yet.");
  }

  return {
    model,
    stream: ui.inferenceStream.checked,
    messages: buildHistoryMessages(entries).concat([{ role: "user", content: prompt }])
  };
}

function parseEventBlock(block: string): string {
  return block
    .split("\n")
    .filter((line) => line.startsWith("data:"))
    .map((line) => line.slice(5).trimStart())
    .join("\n");
}

async function runStreamingRequest(request: Record<string, unknown>): Promise<PendingStreamResult> {
  state.activeController = new AbortController();

  const response = await fetch("/v1/chat/completions", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(request),
    signal: state.activeController.signal
  });

  if (!response.ok) {
    const payload = await readJsonResponse(response);
    throwResponseError(response.status, payload);
  }

  const assistantTurn: TranscriptEntry = { role: "assistant", content: "" };
  state.inferenceTranscript.push(assistantTurn);
  renderTranscript(
    ui.inferenceTranscript,
    state.inferenceTranscript,
    "No inference turns yet. Use a preset or send a custom prompt."
  );

  const reader = response.body?.getReader();
  if (!reader) {
    throw new Error("Streaming response did not expose a readable body.");
  }

  const decoder = new TextDecoder();
  const rawFrames: string[] = [];
  let buffer = "";

  while (true) {
    const { value, done } = await reader.read();
    buffer += decoder.decode(value ?? new Uint8Array(), { stream: !done });

    let boundary = buffer.indexOf("\n\n");
    while (boundary !== -1) {
      const block = buffer.slice(0, boundary);
      buffer = buffer.slice(boundary + 2);
      const data = parseEventBlock(block);
      if (data) {
        rawFrames.push(`data: ${data}`);
        if (data !== "[DONE]") {
          const payload = JSON.parse(data) as {
            choices?: Array<{ delta?: { content?: string } }>;
          };
          const content = payload.choices?.[0]?.delta?.content;
          if (typeof content === "string") {
            assistantTurn.content += content;
            renderTranscript(
              ui.inferenceTranscript,
              state.inferenceTranscript,
              "No inference turns yet. Use a preset or send a custom prompt."
            );
          }
        }
      }
      boundary = buffer.indexOf("\n\n");
    }

    state.lastResponseText = rawFrames.join("\n\n");
    updateInspectors();

    if (done) {
      break;
    }
  }

  return {
    content: assistantTurn.content,
    rawFrames: rawFrames.join("\n\n")
  };
}

async function sendInferencePrompt(promptOverride?: string): Promise<void> {
  const prompt = (promptOverride ?? ui.inferencePrompt.value).trim();
  if (!prompt) {
    setStatus("Enter an inference prompt before sending.", "error");
    return;
  }

  try {
    const request = buildCompletionRequest(prompt, state.inferenceTranscript);
    state.inferenceTranscript.push({ role: "user", content: prompt });
    if (!promptOverride) {
      ui.inferencePrompt.value = "";
    }
    beginActivity(request);
    renderTranscript(
      ui.inferenceTranscript,
      state.inferenceTranscript,
      "No inference turns yet. Use a preset or send a custom prompt."
    );

    if (ui.inferenceStream.checked) {
      setStatus("Streaming /v1/chat/completions.", "neutral");
      await runStreamingRequest(request);
    } else {
      setStatus("Sending /v1/chat/completions.", "neutral");
      const response = await fetchJson<ChatCompletionResponse>("/v1/chat/completions", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(request)
      });
      state.lastResponseText = formatJson(response);
      state.inferenceTranscript.push({ role: "assistant", content: contentFromResponse(response) });
      updateInspectors();
    }

    renderAll();
    setStatus("Inference demo finished successfully.", "success");
  } catch (error) {
    const message = toErrorMessage(error);
    if (state.inferenceTranscript.at(-1)?.role !== "error") {
      state.inferenceTranscript.push({ role: "error", content: message });
    }
    state.lastResponseText = message;
    renderAll();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function createMemorySession(): Promise<void> {
  if (!state.health.sessions.enabled) {
    state.lastMemorySessionError = "Sessions are disabled for this server.";
    setStatus("Sessions are disabled for this server.", "error");
    return;
  }

  const payload: Record<string, unknown> = { model: selectedModel() };
  const systemPrompt = ui.memorySystemPrompt.value.trim();
  if (systemPrompt) {
    payload.system_prompt = systemPrompt;
  }

  beginActivity(payload);
  try {
    const session = await fetchJson<SessionSummary>("/v1/sessions", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(payload)
    });
    state.memorySession = session;
    state.memoryTranscript = [];
    state.lastMemorySessionError = null;
    state.lastResponseText = formatJson(session);
    renderAll();
    setStatus(`Created session ${session.id}.`, "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.lastMemorySessionError = message;
    state.lastResponseText = message;
    updateInspectors();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function refreshMemorySession(): Promise<void> {
  if (!state.memorySession) {
    setStatus("Create a session before refreshing its metadata.", "error");
    return;
  }

  const path = `/v1/sessions/${encodeURIComponent(state.memorySession.id)}`;
  beginActivity(`GET ${path}`);
  try {
    const session = await fetchJson<SessionSummary>(path);
    state.memorySession = session;
    state.lastResponseText = formatJson(session);
    renderAll();
    setStatus(`Fetched ${session.id}.`, "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.lastResponseText = message;
    updateInspectors();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function deleteMemorySession(): Promise<void> {
  if (!state.memorySession) {
    setStatus("There is no active session to delete.", "error");
    return;
  }

  const path = `/v1/sessions/${encodeURIComponent(state.memorySession.id)}`;
  beginActivity(`DELETE ${path}`);
  try {
    await fetchJson<void>(path, { method: "DELETE" });
    state.memorySession = null;
    state.memoryTranscript = [];
    state.lastResponseText = "204 No Content";
    renderAll();
    setStatus("Deleted the active session.", "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.lastResponseText = message;
    updateInspectors();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function ensureMemorySession(): Promise<void> {
  if (state.memorySession) {
    return;
  }
  await createMemorySession();
  if (!state.memorySession) {
    throw new Error(state.lastMemorySessionError ?? "Unable to create a live session.");
  }
}

async function loadMemorySessionSummary(): Promise<void> {
  if (!state.memorySession) {
    return;
  }

  const path = `/v1/sessions/${encodeURIComponent(state.memorySession.id)}`;
  state.memorySession = await fetchJson<SessionSummary>(path);
}

async function sendMemoryPrompt(promptOverride?: string): Promise<void> {
  const prompt = (promptOverride ?? ui.memoryPrompt.value).trim();
  if (!prompt) {
    setStatus("Enter a memory prompt before sending.", "error");
    return;
  }

  try {
    await ensureMemorySession();
    const request = {
      model: selectedModel(),
      stream: false,
      session_id: state.memorySession?.id,
      messages: [{ role: "user", content: prompt }]
    };
    state.memoryTranscript.push({ role: "user", content: prompt });
    if (!promptOverride) {
      ui.memoryPrompt.value = "";
    }
    beginActivity(request);
    renderTranscript(
      ui.memoryTranscript,
      state.memoryTranscript,
      "Session turns will appear here once you create a session and start the guided demo."
    );

    const response = await fetchJson<ChatCompletionResponse>("/v1/chat/completions", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(request)
    });

    state.memoryTranscript.push({ role: "assistant", content: contentFromResponse(response) });
    state.lastResponseText = formatJson(response);
    await loadMemorySessionSummary();
    renderAll();
    setStatus("Memory demo finished successfully.", "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.memoryTranscript.push({ role: "error", content: message });
    state.lastResponseText = message;
    renderAll();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function sendToolPrompt(promptOverride?: string): Promise<void> {
  const prompt = (promptOverride ?? ui.toolsPrompt.value).trim();
  if (!prompt) {
    setStatus("Enter a tool-oriented prompt before sending.", "error");
    return;
  }

  const request = {
    model: selectedModel(),
    stream: false,
    messages: [{ role: "user", content: prompt }]
  };

  beginActivity(request);
  if (!promptOverride) {
    ui.toolsPrompt.value = "";
  }

  try {
    const response = await fetchJson<ChatCompletionResponse>("/v1/chat/completions", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(request)
    });

    state.toolsResponse = contentFromResponse(response) || "// Empty assistant response.";
    state.toolInvocations = response.tool_invocations ?? [];
    state.lastResponseText = formatJson(response);
    renderAll();
    setStatus("Tool demo finished successfully.", "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.toolsResponse = message;
    state.toolInvocations = [];
    state.lastResponseText = message;
    renderAll();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function refreshMetrics(): Promise<void> {
  beginActivity("GET /metrics");
  try {
    state.metrics = await fetchJson<MetricsResponse>("/metrics");
    state.lastResponseText = formatJson(state.metrics);
    renderAll();
    setStatus("Metrics refreshed.", "success");
  } catch (error) {
    const message = toErrorMessage(error);
    state.lastResponseText = message;
    updateInspectors();
    setStatus(message, "error");
  } finally {
    finishActivity();
  }
}

async function refreshAtlas(): Promise<void> {
  setStatus("Refreshing health, models, tools, and metrics.", "neutral");
  try {
    const [healthResult, modelsResult, toolsResult, metricsResult] = await Promise.allSettled([
      fetchJson<HealthPayload>("/healthz"),
      fetchJson<ModelsResponse>("/v1/models"),
      fetchJson<ToolsResponse>("/v1/tools"),
      fetchJson<MetricsResponse>("/metrics")
    ]);

    if (healthResult.status !== "fulfilled") {
      throw healthResult.reason;
    }

    state.health = healthResult.value;
    if (modelsResult.status === "fulfilled") {
      state.models = Array.isArray(modelsResult.value.data) ? modelsResult.value.data : [];
    }
    if (toolsResult.status === "fulfilled") {
      state.tools = Array.isArray(toolsResult.value.data) ? toolsResult.value.data : [];
    }
    if (metricsResult.status === "fulfilled") {
      state.metrics = metricsResult.value;
    }

    renderAll();
    setStatus("Capability Atlas refreshed.", "success");
  } catch (error) {
    setStatus(toErrorMessage(error), "error");
  }
}

function setActiveChapter(chapterId: ChapterId, updateHash: boolean): void {
  state.currentChapter = chapterId;
  for (const [id, link] of navLinks) {
    link.dataset.active = String(id === chapterId);
  }
  for (const [id, section] of chapterSections) {
    section.dataset.active = String(id === chapterId);
  }

  if (updateHash && window.location.hash !== `#${chapterId}`) {
    history.replaceState(null, "", `#${chapterId}`);
  }
}

function syncScrollProgress(): void {
  const scrollable = document.documentElement.scrollHeight - window.innerHeight;
  const progress = scrollable > 0 ? window.scrollY / scrollable : 0;
  ui.progressBarValue.style.transform = `scaleX(${Math.min(1, Math.max(0, progress))})`;
}

function setupObservers(): void {
  const sections = chapterIds
    .map((chapterId) => chapterSections.get(chapterId))
    .filter((section): section is HTMLElement => section !== undefined);

  const chapterObserver = new IntersectionObserver(
    (entries) => {
      let bestEntry: IntersectionObserverEntry | null = null;
      for (const entry of entries) {
        if (!entry.isIntersecting) {
          continue;
        }
        if (!bestEntry || entry.intersectionRatio > bestEntry.intersectionRatio) {
          bestEntry = entry;
        }
      }

      if (bestEntry) {
        setActiveChapter(bestEntry.target.getAttribute("data-chapter") as ChapterId, true);
      }
    },
    {
      rootMargin: "-18% 0px -22% 0px",
      threshold: [0.32, 0.56, 0.8]
    }
  );

  for (const section of sections) {
    chapterObserver.observe(section);
  }

  const revealObserver = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          (entry.target as HTMLElement).classList.add("is-visible");
        }
      }
    },
    { rootMargin: "0px 0px -15% 0px", threshold: 0.1 }
  );

  for (const element of document.querySelectorAll<HTMLElement>("[data-reveal]")) {
    revealObserver.observe(element);
  }
}

function scrollToHash(hash: string): void {
  if (!hash) {
    return;
  }
  const chapterId = hash.slice(1) as ChapterId;
  const target = document.getElementById(chapterId);
  if (target) {
    target.scrollIntoView({ behavior: "smooth", block: "start" });
    setActiveChapter(chapterId, false);
  }
}

function bindPresetButtons(): void {
  for (const button of document.querySelectorAll<HTMLButtonElement>("[data-inference-preset]")) {
    button.addEventListener("click", () => {
      const prompt = button.dataset.inferencePreset ?? "";
      ui.inferencePrompt.value = prompt;
      void sendInferencePrompt(prompt);
    });
  }

  for (const button of document.querySelectorAll<HTMLButtonElement>("[data-memory-preset]")) {
    button.addEventListener("click", () => {
      const prompt = button.dataset.memoryPreset ?? "";
      ui.memoryPrompt.value = prompt;
      void sendMemoryPrompt(prompt);
    });
  }

  for (const button of document.querySelectorAll<HTMLButtonElement>("[data-tools-preset]")) {
    button.addEventListener("click", () => {
      const prompt = button.dataset.toolsPreset ?? "";
      ui.toolsPrompt.value = prompt;
      void sendToolPrompt(prompt);
    });
  }
}

function stopStreaming(): void {
  if (!state.activeController) {
    return;
  }
  state.activeController.abort();
  state.activeController = null;
  setStatus("Streaming request aborted from the browser.", "error");
  syncControls();
}

function bindEvents(): void {
  ui.refreshAllButton.addEventListener("click", () => {
    void refreshAtlas();
  });
  ui.inferenceSend.addEventListener("click", () => {
    void sendInferencePrompt();
  });
  ui.inferenceStop.addEventListener("click", stopStreaming);
  ui.inferenceClear.addEventListener("click", () => {
    state.inferenceTranscript = [];
    renderAll();
    setStatus("Cleared the inference transcript.", "success");
  });
  ui.memoryCreateSession.addEventListener("click", () => {
    void createMemorySession();
  });
  ui.memoryRefreshSession.addEventListener("click", () => {
    void refreshMemorySession();
  });
  ui.memoryDeleteSession.addEventListener("click", () => {
    void deleteMemorySession();
  });
  ui.memorySend.addEventListener("click", () => {
    void sendMemoryPrompt();
  });
  ui.toolsSend.addEventListener("click", () => {
    void sendToolPrompt();
  });
  ui.operationsRefreshMetrics.addEventListener("click", () => {
    void refreshMetrics();
  });
  ui.inferencePrompt.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void sendInferencePrompt();
    }
  });
  ui.memoryPrompt.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void sendMemoryPrompt();
    }
  });
  ui.toolsPrompt.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void sendToolPrompt();
    }
  });

  window.addEventListener("scroll", syncScrollProgress, { passive: true });
  window.addEventListener("hashchange", () => {
    scrollToHash(window.location.hash);
  });
}

function initialize(): void {
  bindPresetButtons();
  bindEvents();
  setupObservers();
  renderAll();
  syncScrollProgress();
  requestAnimationFrame(() => {
    ui.body.classList.add("is-loaded");
  });
  if (window.location.hash) {
    scrollToHash(window.location.hash);
  } else {
    setActiveChapter("overview", false);
  }
  void refreshAtlas();
}

initialize();
