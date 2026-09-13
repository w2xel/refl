# Observation

`Observed` retains a `DispatchHandle` and an explicit non-throwing error sink.
It invokes the same checked targets as runtime and typed calls. It does not modify
the source's replacement chain.

```text
prepare listener snapshot → resolve and validate → invoke and capture result
                         → deliver completion event → extract typed result
```

## Delivery contract

| Situation | Behavior |
| --- | --- |
| Successful erased result capture | Synchronous after-only event |
| Validation failure or target exception | No completion event |
| Result capture throws | Target effects remain; no event |
| Typed extraction throws | Event has already been delivered |
| Listener throws | Error sink receives exception; delivery continues |
| Subscribe during target execution or delivery | Starts with the next call, including a nested call |
| Unsubscribe during delivery | Inactive listeners are skipped |
| Recursive invocation | Nested delivery runs synchronously |

The listener snapshot is allocated before target entry. `Subscription` owns the
registration; destruction or `unsubscribe()` disables it. Tokens refer weakly to
listener state. Retain a token to keep the subscription active.

Events expose member identity and read-only receiver, post-call argument, and
result views. Consumed arguments can be moved from. Views are call-scoped unless
an available anchor is retained. Retention does not freeze values: later extraction
can move from result storage. A listener can also invalidate a borrow through
another mutable handle.

`try_call_retained<T>` uses the same delivery path and keeps the selected result
anchor even if a listener replaces its target. Raw reference export is checked
before target entry.

## Interception boundary

Only calls through the observed adapter emit its events. Direct native access,
`dynamic->method()`, and other proxies bypass it. A live source handle follows
replacement and reset while subscriptions remain attached to member IDs.
Concurrent invocation or subscription changes require external synchronization.

`Hooks<T>` remains a compatibility facade that wraps dynamic targets. Its payloads
are owned copies; non-copyable method results use `Observed`. It is separate from
the subscription adapter.

## Remaining integration

Dedicated property events and a generated typed observation facade are pending.
The current event has no operation-kind field. No automatic old/new property
snapshots or direct-access interception are implemented.

`call_contract` covers listener order, failures, reentry, and retained results.
`dynamic_state` covers replacement during delivery. See the
[tested usage](../getting-started/observation.md).
