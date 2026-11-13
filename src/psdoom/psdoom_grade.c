/*
 * psDoom -- grade -> monster tier (script backend). See psdoom_grade.h.
 */

#include "psdoom_grade.h"

int psd_grade_tier(double grade, int n)
{
    int t;

    if (n <= 1)
    {
        return 0;
    }
    if (!(grade > 0.0))   /* also catches NaN */
    {
        return 0;
    }
    if (grade >= 1.0)
    {
        return n - 1;
    }
    t = (int) (grade * (double) n);   /* floor: grade * n is in [0, n) */
    return (t > n - 1) ? n - 1 : t;
}

int psd_grade_pick(double grade, const int *drawable, int ntiers)
{
    int ndraw = 0;
    int want;
    int i;

    if (drawable == 0 || ntiers <= 0)
    {
        return 0;
    }
    for (i = 0; i < ntiers; i++)
    {
        if (i == 0 || drawable[i])
        {
            ndraw++;
        }
    }
    want = psd_grade_tier(grade, ndraw);
    for (i = 0; i < ntiers; i++)
    {
        if (i == 0 || drawable[i])
        {
            if (want == 0)
            {
                return i;
            }
            want--;
        }
    }
    return 0;   /* unreachable: want < ndraw */
}
