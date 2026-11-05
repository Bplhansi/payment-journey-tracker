# One image for the whole demo: the C++ engine plus the Python parts (simulator, ingest,
# API, dashboard). Built in two stages so the compilers stay out of the final image.

# --- Stage 1: build the C++ engine --------------------------------------------------------
FROM python:3.12-slim-trixie AS engine-build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make git ca-certificates libhiredis-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY engine/ engine/
RUN cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPJT_WITH_REDIS=ON -DPJT_BUILD_TESTS=OFF \
 && cmake --build build -j"$(nproc)" \
 && mkdir -p /out/lib \
 && cp build/pjt_stream build/pjt_replay /out/ \
 && cp -L "$(ldd build/pjt_stream | awk '/libhiredis/ {print $3}')" /out/lib/

# --- Stage 2: runtime ----------------------------------------------------------------------
FROM python:3.12-slim-trixie
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1
WORKDIR /app
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY --from=engine-build /out/lib/ /usr/local/lib/
COPY --from=engine-build /out/pjt_stream /out/pjt_replay /usr/local/bin/
RUN ldconfig
COPY config/ config/
COPY sim/ sim/
COPY api/ api/
COPY dashboard/ dashboard/
COPY scripts/ scripts/
COPY pyproject.toml .
# Run as a normal user, not root. data/ is filled at run time (a volume).
RUN useradd --create-home --uid 1000 pjt \
 && mkdir -p data \
 && chown pjt:pjt data
USER pjt
EXPOSE 8000 8501
CMD ["python", "-m", "uvicorn", "api.app:app", "--host", "0.0.0.0", "--port", "8000"]
