#include <stdio.h>

int main()
{
    int a, b;

    printf("Enter two numbers: ");
    scanf("%d %d", &a, &b);

    if (a = b)          /* Violation 1: Assignment in condition */
    {
        printf("Numbers are equal\n");
    }

    if (a > b)
        printf("Largest = %d\n", a);   /* Violation 2: No braces */

    else
        printf("Largest = %d\n", b);   /* Violation 3: No braces */

    return 0;
}