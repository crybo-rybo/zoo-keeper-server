# ADR-003: Drogon HTTP Framework

## Status
Accepted

## Context
The server needs an HTTP framework that supports async request handling, Server-Sent Events (SSE) for streaming, and integrates cleanly with CMake's FetchContent. Options considered: Drogon, Crow, cpp-httplib, Pistache.

## Decision
Use Drogon as the HTTP framework. It is fetched at build time via CMake FetchContent (no manual installation).

## Consequences
- **Async model:** Drogon uses an event-loop model with callback-based route handlers. Routes are registered as lambdas on `HttpAppFramework`.
- **SSE support:** Drogon's `HttpResponse::newStreamResponse()` enables chunked streaming for the chat completions endpoint.
- **Integration point:** `HttpAppFramework::instance()` is the singleton entry point. Server lifecycle (bind, run, quit) is managed through it in `http_server.cpp`.
- **Connection tracking:** Drogon exposes `trantor::TcpConnectionPtr` for connection lifecycle callbacks, which powers the `DisconnectRegistry` for client-disconnect cancellation.
- **Build:** No system package required — FetchContent handles everything. This simplifies CI and new-developer setup.
