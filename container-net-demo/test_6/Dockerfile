# Start from a base image that has the C compiler (gcc)
FROM gcc:latest

# Set a working directory inside the container
WORKDIR /app

# Copy our C code into the container's /app directory
COPY relay.c .

# Compile the C code and create an executable named "relay"
RUN gcc -pthread -o relay  relay.c

# Set the default command to run when the container starts
# This "exec form" runs ./relay as PID 1
CMD ["./relay"]
