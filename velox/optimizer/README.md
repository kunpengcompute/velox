# Expression e-graph optimizer: stage one

This stage adds ENode/EClass/EGraph, bounded rewriting, cost-based extraction
and an ExprSet entry point. No optimization rule is registered by default.
The empty pass returns the original TypedExpr pointers before inspecting
metadata or constructing a graph, including for lambdas and opaque calls.

Set `optimizer.enabled=false` in QueryConfig to bypass the entry point.
The matching Gluten adapter forwards the Spark session key
`spark.gluten.sql.columnar.backend.velox.optimizer.enabled`.
The process flag `--velox_optimizer_enabled=false` also disables rewriting.

`velox_optimizer_tests` covers graph invariants, input bindings, exact constant
identity, cycle/overflow guards, cost extraction and empty-pass behavior.
Stage two adds EqualChainSwitchToIn, signature checks, execution-pool lookup
arrays and the Spark ExprSet execution tests.
