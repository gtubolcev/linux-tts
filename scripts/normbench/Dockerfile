FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip python3-venv python3-dev build-essential git ca-certificates && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /v && /v/bin/pip install --no-cache-dir ru-normalizr nemo_text_processing
RUN /v/bin/pip install --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu && /v/bin/pip install --no-cache-dir git+https://github.com/Den4ikAI/runorm.git
