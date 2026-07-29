# Multi-stage build: compile a static CGO_ENABLED=0 binary, then ship it in
# a distroless static image (no shell, no package manager - smallest
# reasonable attack surface for a stateless internet-facing UDP/HTTP
# service). Go stdlib only (see go.mod) - no dependency fetch is needed at
# build time.

FROM golang:1.26 AS build
WORKDIR /src
COPY go.mod ./
COPY *.go ./
ENV CGO_ENABLED=0
RUN go build -trimpath -ldflags="-s -w" -o /out/master-server .

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/master-server /master-server
# UDP masterAnnounce/masterQuery traffic, and the HTTP /servers + /healthz
# endpoint. Override with -udp-port/-http-port if you remap these.
EXPOSE 11756/udp
EXPOSE 8080/tcp
ENTRYPOINT ["/master-server"]
