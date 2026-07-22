# Dependencies

| Dep | Visibility | Sync | Notes |
|-----|------------|------|--------|
| **braincube** | **private** research | git submodule `third_party/braincube` | LHLAM core; proprietary LICENSE |
| monocypher (in-tree) | bundled | `libs/crypto` | crypto |
| curl | system | PATH | LLM HTTP |

## Updating braincube
```bash
cd third_party/braincube && git pull origin main && cd ../..
git add third_party/braincube
git commit -m "deps: bump braincube"
```

Do **not** vendor a divergent fork without upstreaming to `Abyss-c0re/braincube`.
