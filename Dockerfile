# Start from a base image that has the C compiler (gcc)
FROM gcc:latest

# Set a working directory inside the container
WORKDIR /app

# Copy our C code into the container's /app directory
COPY main.c .

# Compile the C code and create an executable named "main"
RUN gcc -pthread -o main  main.c

# Dockerfile default (used only if compose/runtime doesn't override)
ENV TOTAL_ROUNDS=1

# Set the default command to run when the container starts
# This "exec form" runs ./main as PID 1
CMD ["./main"]
