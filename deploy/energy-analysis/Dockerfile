FROM python:3.12-slim
# tzdata so zoneinfo can resolve America/Toronto for month boundaries
RUN apt-get update && apt-get install -y --no-install-recommends tzdata \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY analyze.py /app/analyze.py
ENTRYPOINT ["python", "/app/analyze.py"]
