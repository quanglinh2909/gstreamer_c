#include <stdio.h>
#include <stdlib.h>

int read_data_from_file(const char *path, char **out_data)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("fopen %s fail!\n", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    int file_size = (int)ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }

    char *data = (char *)malloc((size_t)file_size + 1);
    if (!data) {
        fclose(fp);
        return -1;
    }
    data[file_size] = 0;

    fseek(fp, 0, SEEK_SET);
    if (file_size != (int)fread(data, 1, (size_t)file_size, fp)) {
        printf("fread %s fail!\n", path);
        free(data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *out_data = data;
    return file_size;
}
