# Start from a base image that has the C compiler (gcc)
FROM gcc:latest

# Set a working directory inside the container
WORKDIR /app

# Copy our C code into the container's /app directory
COPY relay .

# Set the default command to run when the container starts
CMD ["./relay"]