#include <iostream>
#include <string>

struct Session {
    std::string name;
    int value;
};

static int read_session_value(Session *session) {
    return session->value;
}

static int handle_request(Session *session) {
    return read_session_value(session);
}

int main() {
    std::cout << "starting segfault example\n";
    Session *session = nullptr;
    return handle_request(session);
}

