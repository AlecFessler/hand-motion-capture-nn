#!/bin/bash

PORTS=(12345 12346 12347)
COUNTER_FILE=".video_counter.txt"

if [ ! -f "$COUNTER_FILE" ]; then
  echo "0" > "$COUNTER_FILE"
fi

next_counter() {
  counter=$(cat "$COUNTER_FILE")
  next_counter=$((counter + 1))
  echo "$next_counter" > "$COUNTER_FILE"
  echo "$next_counter"
}

case $1 in
  start)
    echo "Starting FFmpeg instances..."
    counter=$(next_counter)
    for port in "${PORTS[@]}"; do
      filename="port${port}_vid${counter}"
      echo "FILENAME=/home/alecfessler/Documents/mimic_dataset/port${port}_vid${counter}_%03d.mp4" > /etc/default/ffmpeg-stream
      sudo systemctl start ffmpeg-stream@"$port".service
      echo "Started ffmpeg-stream@$port.service with filename $filename"
    done
    ;;
  stop)
    echo "Stopping FFmpeg instances..."
    for port in "${PORTS[@]}"; do
      sudo systemctl stop ffmpeg-stream@"$port".service
      echo "Stopped ffmpeg-stream@$port.service"
    done
    ;;
  status)
    echo "Checking status of FFmpeg instances..."
    for port in "${PORTS[@]}"; do
      sudo systemctl status ffmpeg-stream@"$port".service
    done
    ;;
  restart)
    echo "Restarting FFmpeg instances..."
    counter=$(next_counter)
    for port in "${PORTS[@]}"; do
      filename="port${port}_vid${counter}"
      echo "FILENAME=/home/alecfessler/Documents/mimic_dataset/port${port}_vid${counter}_%03d.mp4" > /etc/default/ffmpeg-stream
      sudo systemctl restart ffmpeg-stream@"$port".service
      echo "Restarted ffmpeg-stream@$port.service with filename $filename"
    done
    ;;
  *)
    echo "Usage: $0 {start|stop|status|restart}"
    exit 1
    ;;
esac
