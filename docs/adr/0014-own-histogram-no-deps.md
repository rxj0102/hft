# ADR 14: Own log-linear histogram, no third-party dependencies in the core

Status: Accepted

## Context
The record path must be allocation-free and header-only, and the build must work in any container.

## Decision
About 80 lines, 6 sub-bucket bits, fixed memory. GoogleTest and Google Benchmark were deferred; tests use a small check macro.

## Consequences
Reproducible build. Costs: about 1.6% relative bucket error, and no GoogleTest ecosystem until the vcpkg wiring is done.

## Evidence
test_core histogram checks on known data.
