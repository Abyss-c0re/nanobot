# syntax=docker/dockerfile:1
# Zero network package install — works offline once base image is local.
# Host binary must match image arch (make host on that platform).
#
#   make host && docker build -f Docker/Dockerfile -t nanobot:local .
#   RUNTIME_IMAGE=python:3.12-slim  (default; has python3 + openssl)

ARG RUNTIME_IMAGE=python:3.12-slim
FROM ${RUNTIME_IMAGE}

RUN mkdir -p /opt/nanobot/bin /opt/nanobot/scripts /opt/nanobot/modules /home/nanobot/.nanobot \
 && ln -sfn python3 /usr/local/bin/python || true

COPY build/host/nanobot /opt/nanobot/bin/nanobot
COPY scripts/peer_mcp_bridge.py /opt/nanobot/scripts/peer_mcp_bridge.py
COPY scripts/run.sh /opt/nanobot/scripts/run.sh
COPY Docker/entrypoint.sh /opt/nanobot/entrypoint.sh
COPY Docker/modules/ /opt/nanobot/modules/
COPY Docker/bin/http /usr/local/bin/http

RUN chmod 755 /opt/nanobot/bin/nanobot /opt/nanobot/entrypoint.sh \
      /opt/nanobot/modules/*.sh /opt/nanobot/scripts/run.sh /usr/local/bin/http \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot \
 && ln -sfn /opt/nanobot/bin/nanobot /usr/local/bin/nanobot-mcp \
 && ln -sfn /usr/local/bin/http /usr/local/bin/curl

ENV HOME=/home/nanobot \
    NANOBOT_HOME=/home/nanobot/.nanobot \
    PATH=/opt/nanobot/bin:/usr/local/bin:/usr/bin:/bin \
    NANOBOT_PORT=8787 \
    NANOBOT_MODE=peer \
    WORKSPACE=/home/nanobot/workspace

WORKDIR /home/nanobot
EXPOSE 8787 8788
USER root
ENTRYPOINT ["/opt/nanobot/entrypoint.sh"]
CMD ["peer"]
