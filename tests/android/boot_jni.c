#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

int test_guest(FILE *log);
JNIEXPORT jstring JNICALL Java_org_kyujo2_boottest_BootTest_runTest(JNIEnv *env, jclass cls) {
    char *text = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&text, &size);
    if (!stream) return (*env)->NewStringUTF(env, "Could not allocate the test log");
    int status = test_guest(stream);
    fclose(stream);
    jstring result = (*env)->NewStringUTF(env, size ? text : (status ? "FAIL" : "PASS"));
    free(text);
    return result;
}
