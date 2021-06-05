#include "defs.h"

__kernel
void activation(int size,__global dtype *a,int a_offset, __global dtype *c,int c_offset)
{
    int pos = get_global_id(0);
    if(pos >= size)
        return;
    a+=a_offset;
    c+=c_offset;
    c[pos] = ACTIVATION_F(a[pos]);
}


__kernel
void activation_diff(int size,__global dtype *y,int y_offset, __global dtype *dy,int dy_offset,__global dtype *dx,int dx_offset)
{
    int pos = get_global_id(0);
    if(pos >= size)
        return;
    y+=y_offset;
    dy+=dy_offset;
    dx+=dx_offset;
    dtype y_val  = y[pos];
    dtype dy_val = dy[pos];
    dx[pos] = ACTIVATION_FINV(y_val,dy_val);
}


