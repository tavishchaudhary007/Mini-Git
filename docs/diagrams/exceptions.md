# Exception Hierarchy

All Mini-Git errors derive from `MiniGitException` (itself a `std::runtime_error`). Each subclass overrides `category()`, which the CLI prints as `error [category]: message` and the server turns into an HTTP error.

```mermaid
classDiagram
    class runtime_error {
        <<std>>
        +what() const char*
    }
    class MiniGitException {
        +category() const char*
    }
    class NotARepoException { +category() "repository" }
    class FileNotFoundException { +category() "file" }
    class InvalidObjectException { +category() "object-store" }
    class BranchException { +category() "branch" }
    class NothingToCommitException { +category() "commit" }
    class CheckoutException { +category() "checkout" }
    class MergeException { +category() "merge" }
    runtime_error <|-- MiniGitException
    MiniGitException <|-- NotARepoException
    MiniGitException <|-- FileNotFoundException
    MiniGitException <|-- InvalidObjectException
    MiniGitException <|-- BranchException
    MiniGitException <|-- NothingToCommitException
    MiniGitException <|-- CheckoutException
    MiniGitException <|-- MergeException
```

Image version: [png/exceptions.png](png/exceptions.png)

[Back to all diagrams](README.md)
