# Legal

## License
This project is licensed under the **MIT License** — see [LICENSE](LICENSE).

## No affiliation
**nanobot is not affiliated with, endorsed by, or sponsored by** Grok or any
other AI vendor.

### Supported interoperability (optional)
- **Grok account auth** — browser device-code login if *you* choose that backend.
  Unofficial interoperability only (**not affiliated**).
- **llama.cpp** / other **OpenAI-compatible** local servers — point
  `--base-url` / `--offline` at your own runtime.

Third-party names appear only to describe optional backends you configure yourself.

## User responsibilities
- You must have the legal right to use any remote API or account you connect.
- You are responsible for compliance with third-party terms of service.
- This software is provided **as-is**, without warranty — see MIT License.

## Secrets
Do not commit session tokens, peer tokens, API keys, or device credentials.
`.gitignore` excludes typical secret paths under `$NANOBOT_HOME`.

## Privacy
nanobot runs **on hardware you control**. Chat content is sent only to the
backend you configure (optional Grok session or local llama.cpp / OpenAI-compatible).
Authors do not operate a cloud that receives your prompts by default.

## Naming
Do not imply official Grok endorsement. Product name is **nanobot**.
