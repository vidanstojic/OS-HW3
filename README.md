# xv6 Shared Memory System

![C](https://img.shields.io/badge/C-00599C?style=for-the-badge&logo=c&logoColor=white)
![xv6](https://img.shields.io/badge/OS-xv6-000000?style=for-the-badge&logo=linux&logoColor=white)
![Memory Management](https://img.shields.io/badge/Memory-Shared_Objects-F80000?style=for-the-badge&logo=ram&logoColor=white)
![Status](https://img.shields.io/badge/Build-Passing-brightgreen?style=for-the-badge)

This project introduces a robust Inter-Process Communication (IPC) mechanism for the xv6 operating system through the implementation of global shared memory objects. The system allows multiple processes to map the same physical memory pages into their virtual address spaces, enabling high-performance data exchange and synchronization.

## Overview

The Shared Memory System provides a complete API for managing global memory objects that persist independently of the processes that create them. By modifying the kernel's memory management and page table logic, the system ensures that shared regions are correctly mapped, protected, and synchronized across different process contexts.

## Technical Implementation

* **Kernel-Level Object Management**: Developed a global registry for shared memory objects, identified by unique string names, to allow discovery across different processes.
* **Page Table Manipulation**: Modified the virtual memory system to map physical frames into multiple process address spaces, ensuring proper permission handling and isolation.
* **Resource Lifecycle**: Implemented reference counting for shared objects to ensure physical pages are only deallocated when the last process closes its descriptor.
* **Address Space Integration**: Designed logic for dynamic mapping of shared pages into specific virtual addresses, handling alignment and page-fault prevention.

## Core API

### Object Lifecycle Management
* **Initialization**: A system call to create or open a named shared memory object, returning a unique descriptor for subsequent operations.
* **Sizing**: Support for defining and adjusting the physical size of shared memory objects to meet specific data requirements.
* **Cleanup**: Automated and manual closing of descriptors to maintain system resource integrity and prevent memory leaks.

### Memory Mapping Logic
* **Dynamic Mapping**: A sophisticated mapping function that attaches shared physical pages to a process's virtual address space.
* **Access Control**: Support for various mapping flags to define read/write permissions and memory protection levels.
* **Virtual Address Management**: Handles the allocation and verification of virtual address ranges to ensure they do not collide with existing process memory regions (stack, heap, or code).

## Operational Details

The implementation ensures atomic access to the global object table, preventing race conditions during concurrent object creation or deletion. The system is designed to integrate seamlessly with the existing xv6 process model, allowing shared memory to be used alongside standard pipes and signals.
