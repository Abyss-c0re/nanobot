# syntax=docker/dockerfile:1
# Tiny nanobot image: alpine + static binaries (no apt/apk, no python).
# Host: make static && docker build -f Docker/Dockerfile -t nanobot:local .
#
# Sizes (approx): alpine ~4MB + nanobot ~1.2MB + shell_server ~0.8MB ≈ 6–8MB.
# Fat variant (MCP bridge python, openssl): docker build --build-arg VARIANT=fat ...

ARG VARIANT=tiny
ARG RUNTIME_IMAGE=alpine:3.20

# --- tiny (default) ---
FROM ${RUNTIME_IMAGE} AS tiny
# No apk: lab mirrors often offline. Busybox provides sh/tar/wget/nc/base64/comm.
RUN mkdir -p /opt/nanobot/bin /opt/nanobot/scripts /opt/nanobot/modules \
      /home/nanobot/.nanobot /home/nanobot/workspace /run/sshd /etc/ssh /usr/local/bin \
 && ln -sfn /bin/sh /bin/bash
COPY build/static/nanobot /opt/nanobot/bin/nanobot
COPY Docker/bin/shell_server /opt/nanobot/bin/shell_server
COPY Docker/bin/http /usr/local/bin/http
COPY Docker/entrypoint.sh /opt/nanobot/entrypoint.sh
COPY Docker/modules/ /opt/nanobot/modules/
# optional bridge (fat only uses it with python)
COPY scripts/peer_mcp_bridge.py /opt/nanobot/scripts/peer_mcp_bridge.py
COPY scripts/run.sh /opt/nanobot/scripts/run.sh
RUN chmod 755 /opt/nanobot/bin/nanobot /opt/nanobot/bin/shell_server \
      /opt/nanobot/entrypoint.sh /opt/nanobot/modules/*.sh \
      /opt/nanobot/scripts/run.sh /usr/local/bin/http \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot-mcp \
 && ln -sfn /usr/local/bin/http /usr/local/bin/curl
ENV HOME=/home/nanobot NANOBOT_HOME=/home/nanobot/.nanobot \
    PATH=/opt/nanobot/bin:/usr/local/bin:/usr/bin:/bin \
    NANOBOT_PORT=8787 NANOBOT_MODE=peer WORKSPACE=/home/nanobot/workspace \
    SSH_ENABLE=1 SSH_PORT=22 SHELL_SERVER_SHELL=/bin/sh
WORKDIR /home/nanobot
EXPOSE 8787 8788 22
USER root
ENTRYPOINT ["/opt/nanobot/entrypoint.sh"]
CMD ["peer"]

# --- fat: python slim + openssl for bundles + mcp bridge ---
FROM python:3.12-slim AS fat
RUN mkdir -p /opt/nanobot/bin /opt/nanobot/scripts /opt/nanobot/modules \
      /home/nanobot/.nanobot /home/nanobot/workspace /run/sshd /etc/ssh \
 && ln -sfn python3 /usr/local/bin/python || true
# prefer static bin if present, else host dynamic
COPY build/static/nanobot /opt/nanobot/bin/nanobot
COPY scripts/peer_mcp_bridge.py /opt/nanobot/scripts/peer_mcp_bridge.py
COPY scripts/run.sh /opt/nanobot/scripts/run.sh
COPY Docker/entrypoint.sh /opt/nanobot/entrypoint.sh
COPY Docker/modules/ /opt/nanobot/modules/
COPY Docker/bin/http /usr/local/bin/http
COPY Docker/bin/shell_server /opt/nanobot/bin/shell_server
COPY Docker/bin/shell_server.py /opt/nanobot/bin/shell_server.py
RUN chmod 755 /opt/nanobot/bin/nanobot /opt/nanobot/entrypoint.sh \
      /opt/nanobot/modules/*.sh /opt/nanobot/scripts/run.sh \
      /usr/local/bin/http /opt/nanobot/bin/shell_server \
      /opt/nanobot/bin/shell_server.py \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot-mcp \
 && ln -sfn /usr/local/bin/http /usr/local/bin/curl
ENV HOME=/home/nanobot NANOBOT_HOME=/home/nanobot/.nanobot \
    PATH=/opt/nanobot/bin:/usr/local/bin:/usr/bin:/bin \
    NANOBOT_PORT=8787 NANOBOT_MODE=peer WORKSPACE=/home/nanobot/workspace \
    SSH_ENABLE=1 SSH_PORT=22
WORKDIR /home/nanobot
EXPOSE 8787 8788 22
USER root
ENTRYPOINT ["/opt/nanobot/entrypoint.sh"]
CMD ["peer"]

# select final stage
FROM ${VARIANT}
