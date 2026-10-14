#include <stdio.h>
#include <stdlib.h>

extern int protocol_vectors_run_all(void);

int main(void) {
    int failures = protocol_vectors_run_all();
    if (failures > 0) {
        printf("Host tests failed with %d errors.\n", failures);
        return EXIT_FAILURE;
    }
    printf("Host tests passed successfully.\n");
    return EXIT_SUCCESS;
}
