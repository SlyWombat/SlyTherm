FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY slylog_collector ./slylog_collector
CMD ["python", "-m", "slylog_collector.main"]
