FROM docker:cli AS docker-cli

FROM mysql:8.4

COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker

RUN microdnf install -y bash telnet && \
    microdnf clean all

COPY watcher.sh /watcher.sh

RUN chmod +x /watcher.sh

CMD ["/bin/bash", "/watcher.sh"]