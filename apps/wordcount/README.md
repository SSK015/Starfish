# Wordcount

Enable `FARLIB_BUILD_APPLICATIONS=ON` and `FARLIB_BUILD_WORDCOUNT=ON` in the
root CMake build. The main executable uses:

```text
wordcount_far <config> <dataset> [threads] [max_bytes] [top_k]
```

`FARLIB_WORDCOUNT_STARFISH` enables Starfish-specific recomputation hooks
(default ON). The `FARLIB_WORDCOUNT_RECOMPUTABLE` and
`FARLIB_WORDCOUNT_RECIPES` environment flags select the Starfish mode.
Recipe mode requires
`FARLIB_WORDCOUNT_KEEP_LOCAL_FAR_MAPS=1`; workload and map settings come from
the selected configuration.

`FARLIB_BUILD_WORDCOUNT_RECOVERY=ON` additionally builds `wordcount_recovery`
from that same source, with Starfish fault gates and verification enabled.
Normal `wordcount_far` binaries exclude those test sections.

Input text and generated correctness fixtures are not bundled. To generate the
deterministic correctness fixture from the repository root:

```bash
python3 apps/wordcount/testdata/generate_small_oracle.py apps/wordcount/testdata
```

This synthetic fixture is for correctness checking, not a performance baseline.
