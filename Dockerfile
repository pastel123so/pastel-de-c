FROM alpine:3.20 AS build

RUN apk add --no-cache build-base linux-headers
WORKDIR /app
COPY Makefile .
COPY src ./src
RUN mkdir -p resources
COPY resources/references.bin ./resources/references.bin
RUN make && ./build/build-index resources/references.bin resources/references.idx && rm resources/references.bin

FROM alpine:3.20

WORKDIR /app
COPY --from=build /app/build/rinha-api /app/rinha-api
COPY --from=build /app/build/rinha-lb /app/rinha-lb
COPY --from=build /app/resources /app/resources

ENV PORT=8080
ENV REFERENCES_PATH=/app/resources/references.idx

EXPOSE 8080
CMD ["/app/rinha-api"]
