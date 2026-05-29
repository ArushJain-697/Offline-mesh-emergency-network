#  Offline Mesh Emergency Network

This project implements a basic mesh network using UDP sockets in C. It allows nodes to discover each other, send messages to specific nodes, and broadcast messages to the entire network. The core functionality includes node management, network communication, message handling, and history logging. A command-line interface (CLI) is provided for user interaction, and a separate utility is included for configuring the network by adding or updating node information.

##  Key Features

*   **Node Discovery:** Nodes can discover each other and maintain a list of known nodes in the network.
*   **Message Sending:** Users can send messages to specific nodes by specifying the destination node's identifier.
*   **Broadcast Messaging:** Users can broadcast messages to all known nodes in the network.
*   **Message Receiving:** Nodes continuously listen for incoming messages and display them to the user.
*   **History Logging:** Sent and received messages are logged to a history file for each node.
*   **Node Configuration:** A utility program (`add_node.c`) allows for easy addition or modification of node information in the network configuration file (`nodes.dat`).
*   **Command-Line Interface:** User-friendly CLI for interacting with the mesh network.
*   **Multi-threading:** Utilizes a separate thread for receiving messages, allowing for concurrent operation.

##  Tech Stack

*   **Language:** C
*   **Networking:** UDP Sockets (using `arpa/inet.h`, `sys/socket.h`, `netinet/in.h`)
*   **Threading:** POSIX Threads (`pthread.h`)
*   **Data Storage:** Flat file (`nodes.dat`) for storing node information
*   **Build Tools:** Make (assumed, based on typical C projects)
*   **Standard C Libraries:** `stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `ctype.h`, `time.h`, `errno.h`

##  Getting Started / Setup Instructions

### Prerequisites

*   A C compiler (e.g., GCC)
*   POSIX threads library (`pthread`)
*   Make (optional, but recommended for building)

### Installation

1.  **Clone the repository:**
    ```bash
    git clone <repository_url>
    cd <repository_directory>
    ```

2.  **Compile the code:**
    ```bash
    make
    ```

3.  **Create the `nodes.dat` file (if it doesn't exist):**
    The `nodes.dat` file stores information about the nodes in the network. You can create it manually or use the `add_node` utility to add the first node.

### Running Locally

1.  **Configure the nodes:**
    Use the `add_node` utility to add or update node information in the `nodes.dat` file.  For example:
    ```bash
    ./add_node A B 127.0.0.1 5001
    ```
    This command adds a node with the name 'B', IP address '127.0.0.1', and port '5001', using node 'A' as a helper.
    The four positional arguments are: `<HelperNodeLetter> <NewNodeLetter> <NewNodeIP> <NewNodePort>`.

    Option B (Interactive Wizard): You can also use the easier interactive mode provided in the Makefile:
    ```bash
    make add
    ```
    Run the mesh network application: Use the Makefile shortcut to run the chat program:
    ```bash
    make chat
    ```

3.  **Run the mesh network application:**
    ```bash
    ./mesh_cli
    ```

4.  **Interact with the CLI:**
    The CLI will prompt you to enter a node letter and then present a menu of options: send message, broadcast, or exit.

## 💻 Usage

1.  **Start multiple instances of the `mesh_cli` application** on different terminals, each representing a node in the mesh network.
2.  **Configure each node** with a unique name, IP address, and port using the `add_node` utility. Ensure that the `nodes.dat` file is correctly populated with the information for all nodes in the network.
3.  **Use the CLI** to send messages to specific nodes or broadcast messages to the entire network.
4.  **Observe the messages** being received and displayed on the other nodes' terminals.

## 📂 Project Structure

```
├── add_node.c          # Utility to add/update node information in nodes.dat
├── mesh_backend.c      # Core backend logic for the mesh network
├── mesh_backend.h      # Header file for the mesh network backend
├── mesh_cli.c          # Command-line interface for interacting with the mesh network
├── nodes.dat           # (Optional) File storing node information
└── Makefile            # (Optional) Build file
```


