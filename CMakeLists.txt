cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lwm2m_base_client)

target_sources(app PRIVATE
  src/main.c
  src/send_scheduler_core.c
  src/send_scheduler_lwm2m.c
  src/temperature_sensor.c
  src/humidity_sensor.c
)
