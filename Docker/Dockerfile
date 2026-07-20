# syntax=docker/dockerfile:1
# make host && docker build -f Docker/Dockerfile -t nanobot:local .
ARG RUNTIME_IMAGE=python:3.12-slim
FROM ${RUNTIME_IMAGE}
RUN mkdir -p /opt/nanobot/bin /opt/nanobot/scripts /opt/nanobot/modules /home/nanobot/.nanobot /run/sshd /etc/ssh \
 && ln -sfn python3 /usr/local/bin/python || true
COPY build/host/nanobot /opt/nanobot/bin/nanobot
COPY scripts/peer_mcp_bridge.py /opt/nanobot/scripts/peer_mcp_bridge.py
COPY scripts/run.sh /opt/nanobot/scripts/run.sh
COPY Docker/entrypoint.sh /opt/nanobot/entrypoint.sh
COPY Docker/modules/ /opt/nanobot/modules/
COPY Docker/bin/http /usr/local/bin/http
COPY Docker/bin/shell_server.py /opt/nanobot/bin/shell_server.py
RUN chmod 755 /opt/nanobot/bin/nanobot /opt/nanobot/entrypoint.sh \
      /opt/nanobot/modules/*.sh /opt/nanobot/scripts/run.sh \
      /usr/local/bin/http /opt/nanobot/bin/shell_server.py \
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
