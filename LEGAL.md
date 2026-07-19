# Legal notice

**Last updated:** 2026-07-19

## License of this software

nanobot source code and documentation in this repository are licensed under the
**MIT License**. See [LICENSE](LICENSE).

Copyright (c) 2026 nanobot contributors.

## No affiliation (important)

nanobot is an **independent, unofficial project**.

It is **not** affiliated with, endorsed by, sponsored by, or partnered with:

- xAI
- Grok (product or brand)
- SpaceX
- SpaceXAI
- OpenAI, Meta, or any other AI vendor
- Roborock, Xiaomi, or any robot/vacuum manufacturer

References to third-party names, products, APIs, or domains appear **only** to
describe optional interoperability (for example calling a public HTTP API or a
local OpenAI-compatible server such as llama.cpp).

## Trademarks

All third-party trademarks, service marks, product names, and company names
remain the property of their respective owners. Use of those names does **not**
imply endorsement.

## Third-party services and terms

If you configure nanobot to talk to a remote service (including any Grok/xAI
endpoint, auth server, or other cloud API):

1. **You** must have a lawful right to use that service (account, credentials, license).
2. **You** are solely responsible for complying with that service’s terms of use,
   privacy policy, rate limits, and applicable law.
3. nanobot does **not** grant any rights to third-party services or content.
4. Device-code / browser login uses **your** browser session; do not share session
   files (`session`, tokens under `NANOBOT_HOME`).

Local backends (e.g. llama.cpp on your machine) are under **your** control and
licenses for those models/tools.

## Runtime dependencies

nanobot expects a normal system `curl` (or compatible) for HTTPS/HTTP. `curl` and
OpenSSL/TLS stacks are separate projects with their own licenses, installed by
your OS package manager—not redistributed in this repository by default.

Optional cross-compilers under `toolchain/` (if you download them) are third-party
and must not be committed; respect their licenses.

## No warranty / liability

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

This includes, without limitation: account bans, API breakage, data loss,
security issues from running shell tools, or misuse of remote APIs.

## Security and secrets

Do **not** commit:

- `session`, `peer_token`, `env` with secrets, API keys, private keys
- personal memory profiles that contain private data you do not wish to publish

See [SECURITY.md](SECURITY.md).

## Export and use

You are responsible for compliance with export controls and local law in your
jurisdiction when distributing or using this software.

## Contact

For legal concerns about this repository, open an issue on the project host or
contact the maintainers listed in the repository metadata (if any).
