# Legal

## License
This project is licensed under the **MIT License** — see [LICENSE](LICENSE).

## No affiliation
**nanobot is not affiliated with, endorsed by, or sponsored by:**
- xAI, Grok, or any Grok product
- SpaceX or SpaceXAI
- OpenAI, Anthropic, Google, Meta, or other AI vendors
- Roborock, Xiaomi, or any robot vacuum vendor
- LineageOS, Google Android, or Unihertz

Names of third parties appear only to describe optional interoperability
(e.g. browser device-code login with a Grok account, or an OpenAI-compatible
local server such as llama.cpp).

## User responsibilities
- You must have the legal right to use any remote API or account you connect.
- You are responsible for compliance with third-party terms of service.
- This software is provided **as-is**, without warranty — see MIT License.

## Secrets
Do not commit session tokens, peer tokens, API keys, or device credentials.
`.gitignore` excludes typical secret paths under `$NANOBOT_HOME`.

## Privacy
nanobot runs **on hardware you control**. Chat content is sent only to the
backend you configure (Grok cloud session or local LLM). Authors do not operate
a cloud that receives your prompts by default.

## Export / use
MIT permits commercial and private use. Publishing a fork that implies official
xAI/Grok endorsement is not allowed under trademark norms — keep naming clearly
independent (e.g. “nanobot”, not “official Grok agent”).
