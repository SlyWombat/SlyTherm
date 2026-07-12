FROM python:3.12-slim
RUN pip install --no-cache-dir "psycopg[binary]==3.*"
WORKDIR /app
COPY app.py .
EXPOSE 8091
CMD ["python", "app.py"]
