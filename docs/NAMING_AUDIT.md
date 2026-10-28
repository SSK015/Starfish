# Naming audit

This is a candidate list, not an approved rename plan. The audit covers the
standalone runtime at commit `168edda`. It changes no source, configuration,
test, or protocol. Candidate names below describe possible directions; the
final names still need agreement.

Priority 1 means the current name hides its role or retains a design-stage
label. Priority 2 means the scope or compatibility boundary needs more review.
Priority does not imply that a rename is safe to apply immediately.

## Priority 1: clarify the role

1. **`concurrent_cache.hpp` / `ConcurrentArrayCache`.**
   This class coordinates local object allocation, fetch, eviction, placement,
   and fault-tolerance work. It is not an array container. Consider
   `object_cache.hpp` / `ObjectCache` or `FarMemoryCache`, after deciding
   whether the class should represent a cache or a broader runtime coordinator.
   The public `Cache` alias, application references, and included `.ipp`
   method definitions make this a source-API change, not just a file rename.
   Evidence: [class](../runtime/include/mm/cache/concurrent_cache.hpp#L173),
   [alias](../runtime/include/mm/cache/accessor.hpp#L6).

2. **`design1` / `Design1Gain` / `print_design1_diagnostics`.**
   These implement and report behavior-group placement decisions, including
   Resident/Streaming gain ranking. Replace the design number with the
   placement role; possible directions are `placement` and
   `ResidentStreamingGain`. The parsed and printed value `design1_gain`
   is a separate compatibility concern.
   Evidence: [policy and parser](../runtime/include/mm/placement/placement_policy.hpp#L9),
   [planner](../runtime/include/mm/placement/placement_planner.ipp#L25),
   [diagnostics](../runtime/include/mm/placement/diagnostics.ipp#L5).

3. **`design2` namespaces and test names.**
   This label covers different roles. `cache::design2::behavior_group`
   classifies behavior and selects protection. `FarLib::design2` in
   `eviction_commit.hpp` consumes pending-eviction state exactly once.
   Name these separately by protection policy and eviction completion.
   Do not replace every `design2` occurrence with one new module name.
   Tests should describe the behavior they verify.
   Evidence: [policy](../runtime/include/ft/policy/behavior_group_policy.hpp#L7),
   [completion bookkeeping](../runtime/include/mm/evacuator/eviction_commit.hpp#L10),
   [test registration](../tests/design2/CMakeLists.txt#L6).

4. **`simple_region_budget` / `simple_region_heat` /
   `simple_dirty_observer`.**
   These maintain region-supply budgets, demand-access classification, and
   dirty-access observations. `simple` does not explain those roles.
   Possible directions are `region_supply_budget`, `region_access_heat`,
   and `region_dirty_observer`. First establish whether `simple` names
   a baseline or ablation that must remain distinguishable. Existing
   `FARLIB_SIMPLE_*` environment variables and `simple_*` diagnostic
   labels require a migration plan.
   Evidence: [budget modes](../runtime/include/mm/placement/simple_region_budget.hpp#L64),
   [heat and placement labels](../runtime/include/mm/placement/simple_region_heat.hpp#L27),
   [observer](../runtime/include/profiling/simple_dirty_observer.hpp#L3).

5. **`sponge` / `Sponge*`.**
   The name comes from a design-3 prototype. It now appears in the
   single-object erasure-coded stripe mode and commit/acknowledgement messages.
   A descriptive mode name could be `single_object_ec`. This is not a safe
   global substitution: configuration, wire structures, identifiers, and
   client/server compatibility must be considered separately.
   Evidence: [origin](../runtime/include/ft/ec/small_object_stripe.hpp#L5),
   [configuration](../runtime/include/transport/rdma/config.def#L30),
   [commit RPC](../runtime/src/transport/rdma/client.cpp#L385).

6. **`direct_builder` / `direct_owner`.**
   These worker-owned fields are also used by incremental-update and
   read-modify-write paths. The `direct` prefix suggests a narrower scope.
   Consider a group-builder name and an explicit owner/worker identifier,
   such as `stripe_group_builder` and `ec_owner_id`, after checking the
   ownership model. Review the underlying `EcDirectGroupBuilder` name too.
   Evidence: [fields](../runtime/include/mm/cache/concurrent_cache.hpp#L3544),
   [update use](../runtime/include/ft/ec/ec_update_path.ipp#L19),
   [read-modify-write use](../runtime/include/ft/ec/ec_rmw_path.ipp#L98).

7. **`beehive_profile`.**
   This is a workload wrapper that resets profiling state, invokes a function,
   and prints runtime statistics. Its name refers to an earlier project,
   rather than its role here. Consider `profile_workload` or `profile_run`;
   check application call sites before changing the API.
   Evidence: [implementation](../runtime/include/profiling/profile.hpp#L13).

## Priority 2: review scope before choosing a name

8. **`common_path.ipp`.**
   This mixes completion-queue polling, object pin/unpin, dirty marking, and
   scope operations. A single name such as `completion_polling` would be
   incomplete. Review the ownership and split boundaries before naming it.
   Evidence: [polling](../runtime/include/mm/cache/common/common_path.ipp#L7),
   [pin/unpin](../runtime/include/mm/cache/common/common_path.ipp#L184),
   [dirty marking](../runtime/include/mm/cache/common/common_path.ipp#L231).

9. **Other `*_path.hpp` / `*_path.ipp` files.**
   `path` often reflects how implementations were extracted. Review
   `evict_path`, `evacuate_path`, `fetch_path`, `profile_path`,
   `recompute_recipe_path`, the EC paths, and allocator paths such as
   `region_head_path` and `global_heap_path`. Names such as
   `eviction.ipp` or `fetch.ipp` may be clearer for cohesive files;
   large mixed files need a responsibility review first. The `.ipp`
   extension itself is useful and should remain.
   Evidence: [eviction](../runtime/include/mm/evacuator/evict_path.ipp#L6),
   [evacuation](../runtime/include/mm/evacuator/evacuate_path.ipp#L174),
   [allocator](../runtime/include/mm/allocator/global_heap_path.hpp).

10. **`ec_direct` / `FARLIB_EC_DIRECT_WRITE`.**
    Here `direct` includes borrowing the original object as the write source
    and using a fixed parity bank. It does not by itself explain which copy
    or staging step is avoided. Consider a name based on borrowed-source
    writes after checking all uses. Environment and diagnostic names need
    compatibility handling.
    Evidence: [write path](../runtime/include/ft/ec/ec_direct_path.ipp#L11).

11. **`EvacRdmaTraffic::EcBatch` / `traffic=ec_batch`.**
    The traffic bucket also records read-modify-write reads and writes.
    Consider `ErasureCoded` / `ec` for the broad metric, while retaining a
    separate strategy label. The `ec_batch` mode itself has a concrete
    batched-stripe meaning and need not receive the same rename.
    Evidence: [read-modify-write accounting](../runtime/include/ft/ec/ec_rmw_path.ipp#L712),
    [printed metric](../runtime/src/profiling/stats.cpp#L1343).

12. **`async/option.hpp` / global `Algorithm` / `DEFAULT_ALG`.**
    These select execution modes, including user threads and prefetching.
    The file and enum names omit that domain. Consider
    `execution_policy.hpp` / `ExecutionPolicy`, after checking the exact
    meaning of `PARAROUTINE` and the application-facing interface.
    Evidence: [options](../runtime/include/async/option.hpp#L4).

13. **`generic_stream.hpp` / `GenericStreamRunner`.**
    `generic` describes flexibility, not the task-stream execution role.
    Consider `TaskStreamRunner` only after distinguishing it from the
    existing stream runners and checking its public use.
    Evidence: [runner and parameters](../runtime/include/async/generic_stream.hpp#L17).

14. **Build and inherited project names.**
    `project(FarLib)`, `libcache`, `libasync`, and `server`
    deserve a separate build/API naming review. `libcache` links memory,
    fault-tolerance, transport, and profiling code; `libasync` compiles
    `signal.cpp`. Possible directions include `starfish_runtime` and
    `memory_server`. Changing targets affects scripts and consumers.
    The public `FarLib` namespace and `FARLIB_*` options are broader
    compatibility decisions, not automatic branding replacements.
    Evidence: [project](../CMakeLists.txt#L2), [targets](../CMakeLists.txt#L77).

## Keep the meaning; clarify it where needed

- **`fixed_six_runtime` / `six_group`:** six is meaningful. The behavior
  classification has Hot/Cold crossed with Low/Medium/High classes.
  Fixed family slots do not imply that regions can never change groups.
  The Resident/Streaming compatibility encoding has a different interpretation.
- **`list_only_six`:** this is a separate structural ablation with six child
  lists. It does not enable the same region/group metadata. Do not merge its
  name or semantics with behavior classification.
  Evidence: [classes](../runtime/include/ft/policy/behavior_group_policy.hpp#L9),
  [list-only mode](../runtime/include/mm/allocator/fixed_six_runtime.hpp#L60),
  [placement encoding](../runtime/include/mm/placement/simple_region_heat.hpp#L51).
- **Standard mechanisms:** EC (erasure coding), RDMA (remote direct memory
  access), and RMW (read-modify-write) describe real mechanisms. Concurrent
  hash-map names also describe their data structures. They are not in the
  same category as unexplained design numbers.
- **Third-party names:** exclude vendored code and upstream APIs from this
  cleanup, including the Microsoft-origin `lib/zipfian.hpp`.

## Related status text and small clarity issues

Some comments and startup warnings still describe an earlier phase.
`config.def` and the header of `small_object_stripe.hpp` say that the
Sponge write or acknowledgement path is not implemented. `config.hpp` also
prints warnings about missing EC write/acknowledgement paths. Commit and
completion-handling code now exists; reconcile the text with each current mode.
Distinguish the standalone encoder interface from the commit path: a claim
that writes are not routed through that class is narrower than a claim that
the runtime has no writes. The encoder's rebuild interface has callers.
Code presence alone does not establish hardware correctness or full coverage.
Preserve useful prototype provenance.
Evidence: [startup warnings](../runtime/include/transport/rdma/config.hpp#L558),
[encoder comment](../runtime/include/ft/ec/small_object_stripe.hpp#L4356),
[rebuild caller](../runtime/include/ft/recovery/ec_background_rebuild.ipp#L354).

`ShardedScopeCounters` has a `Prototype:` comment, although it is integrated
with the optional sharded scope-accounting path. Review that status label;
the type name already describes sharding. The field `count[2]` could more
explicitly describe active scopes by epoch. This is lower priority than
removing unexplained design labels.
Evidence: [counter](../runtime/include/mm/cache/common/sharded_scope_counters.hpp#L9),
[scope operations](../runtime/include/mm/cache/common/common_path.ipp#L301).

## Rules for a later rename

- Agree on the role and name first. Keep naming-only changes separate from
  changes to behavior or class responsibilities.
- Treat files, includes, types, namespaces, `.ipp` method owners, applications,
  and tests as one source-consistency change.
- Inventory configuration values, environment variables, build targets, wire
  identifiers, and log parsers separately. Define compatibility and deprecation
  before changing an externally consumed name.
- Keep the archived paper artifact separate from ongoing runtime maintenance.
  Validate approved code renames with the normal bounded build and relevant
  tests; this documentation-only audit does not require a build.
