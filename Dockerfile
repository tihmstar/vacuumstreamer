FROM debian:latest
LABEL maintainer="tihmstar <tihmstar@gmail.com>"

RUN apt-get update && \
    apt-get install -y file binutils bsdmainutils clang lldb make && \
    apt-get clean

WORKDIR /workspace/