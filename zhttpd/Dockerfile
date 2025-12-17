
FROM alpine:latest AS builder

RUN apk add --no-cache build-base

WORKDIR /build

COPY . .

RUN gcc zhttpd.c -o zhttpd -std=c11 -O2 -I. -D_POSIX_C_SOURCE=200809L

FROM alpine:latest

RUN adduser -D zuser
USER zuser

WORKDIR /home/zuser/www

COPY --from=builder /build/zhttpd /usr/local/bin/zhttpd

COPY site/ . 

EXPOSE 8080

CMD ["zhttpd", "."]

