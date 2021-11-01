#ifndef REDUCE_DIMS
#error "REDUCE_DIMS must be defined"
#endif
#ifndef DIMS
#error "DIMS must be defined"
#endif

#define NORMAL_DIMS (DIMS - REDUCE_DIMS)

#if REDUCE_DIMS > DIMS
#error "REDUCE_DIMS must be <= DIMS"
#endif
#if REDUCE_DIMS < 0
#error "Need at least 1 dim for reduction"
#endif


typedef struct __attribute__ ((packed)) Shape {
    ulong s[DIMS];
} Shape;

inline ulong get_base_offset(Shape s,Shape strides,ulong offset)
{
    ulong r = offset;
    #pragma unroll
    for(int i=REDUCE_DIMS;i<DIMS;i++) {
        r+= s.s[i]*strides.s[i];
    }
    return r;
}

inline ulong get_reduce_offset(Shape s,Shape strides)
{
    ulong r = 0;
    #pragma unroll
    for(int i=0;i<REDUCE_DIMS;i++) {
        r+= s.s[i]*strides.s[i];
    }
    return r;
}


void next_pos(Shape limits,Shape *pos)
{
#if REDUCE_DIMS == 1
    pos->s[0] ++;
#elif REDUCE_DIMS == 2
    pos->s[1]++;
    if(pos->s[1] == limits.s[1]) {
        pos->s[1] = 0;
        pos->s[0] ++;
    }
#elif REDUCE_DIMS == 3
    pos->s[2]++;
    if(pos->s[2] == limits.s[2]) {
        pos->s[2] = 0;
        pos->s[1] ++;
        if(pos->s[1] == limits.s[1]) {
            pos->s[1] = 0;
            pos->s[0] ++;
        }
    }
#else 
// for total dims limit = 5 shouldn't be more than 3 reduction dims otherwise they will be shrinked
#error "Too many reduction dims"
#endif

}

inline Shape get_pos(Shape limits)
{
    Shape r;
    ulong reduce_item = get_global_id(0) * ITEMS_PER_WI;
#if REDUCE_DIMS == 1
    r.s[0] = reduce_item;
#elif REDUCE_DIMS == 2
    r.s[0] = reduce_item / limits.s[1];
    r.s[1] = reduce_item % limits.s[1];
#elif REDUCE_DIMS == 3
    r.s[2] = reduce_item % limits.s[2];
    ulong ri2 = reduce_item / limits.s[2];
    r.s[1] = ri2 % limits.s[1];
    r.s[0] = ri2 / limits.s[1];
#else 
// for total dims limit = 5 shouldn't be more than 3 reduction dims otherwise they will be shrinked
#error "Too many reduction dims"
#endif

#if NORMAL_DIMS == 0
    // nothing
#elif NORMAL_DIMS == 1
    r.s[REDUCE_DIMS + 0] = get_global_id(1);
#elif NORMAL_DIMS == 2
    r.s[REDUCE_DIMS + 0] = get_global_id(2);      
    r.s[REDUCE_DIMS + 1] = get_global_id(1);      
#elif NORMAL_DIMS == 3
    r.s[REDUCE_DIMS + 0] = get_global_id(2) / limits.s[REDUCE_DIMS+1];      
    r.s[REDUCE_DIMS + 1] = get_global_id(2) % limits.s[REDUCE_DIMS+1];      
    r.s[REDUCE_DIMS + 2] = get_global_id(1);      
#elif NORMAL_DIMS == 4
    r.s[REDUCE_DIMS + 0] = get_global_id(2) / limits.s[REDUCE_DIMS+1];      
    r.s[REDUCE_DIMS + 1] = get_global_id(2) % limits.s[REDUCE_DIMS+1];      
    r.s[REDUCE_DIMS + 2] = get_global_id(1) / limits.s[REDUCE_DIMS+3];
    r.s[REDUCE_DIMS + 3] = get_global_id(1) % limits.s[REDUCE_DIMS+3];
#else
#error "Unsupported dim"
#endif
    return r;
}


inline bool valid_pos(Shape pos,Shape limits)
{
    #pragma unroll
    for(int i=0;i<DIMS;i++)
        if(pos.s[i] >= limits.s[i])
            return 0;
    return 1;

}

#define PARAM_INPUT(type,I) ,__global type const *px##I,ulong px##I##_offset,Shape xstrides##I
#define PARAM_OUTPUT(type,I) ,__global type *py##I,ulong py##I##_offset,Shape ystrides##I
#define PAPAM_WEIGHT(type,I) ,type w##I


#define PREPARE_LOAD_INPUT(type,I) \
    ulong input_offset_##I = get_base_offset(index,xstrides##I,px##I##_offset); \
    type x##I;

#define LOAD_INPUT(I) x##I = px##I[input_offset_##I + get_reduce_offset(index,xstrides##I)];
#define SAVE_OUTPUT(I) py##I[get_base_offset(index,ystrides##I,py##I##_offset)] = reduce_y##I;

#define my_get_local_wg_id() ((get_local_id(2) * get_local_size(1) * get_local_size(0)) + (get_local_id(1) * get_local_size(0)) + get_local_id(0))

#define REDUCE_INIT(type,I) \
    __local type my_reduce_##I[WG_SIZE]; \
    type reduce_y##I,y##I; 

#define SAVE_REDUCE(I) my_reduce_##I[lid] = reduce_y##I;
#define LOAD_REDUCE(I) reduce_y##I = my_reduce_##I[lid]; y##I = my_reduce_##I[nxt];

#define LOAD_REDUCED_SAVE_GLOBAL(I) y##I = my_reduce_##I[0]; py##I[get_base_offset(index,ystrides##I,py##I##_offset)] = y##I;


__kernel 
__attribute__((reqd_work_group_size(WG_SIZE,1,1)))
void exec(Shape limit 
                   PARAMS)

{
    Shape index0 = get_pos(limit);
    Shape index = index0;
    PREPARE_LOAD_INPUT_ALL
    REDUCE_INIT_ALL

    #pragma unroll(8)
    for(int item=0;item < ITEMS_PER_WI;item++) {
        if(valid_pos(index,limit)) {
            LOAD_INPUT_ALL
            CALC
            REDUCE
        }
#if ITEMS_PER_WI > 1
        next_pos(limit,&index);
#endif
    }

    int lid = my_get_local_wg_id(); 

    SAVE_REDUCE_ALL
    
    barrier(CLK_LOCAL_MEM_FENCE); 
    for(int i= WG_SIZE / 2;i>0; i>>= 1) { 
        if(lid < i) { 
            int nxt = lid+i;
            LOAD_REDUCE_ALL
            REDUCE
            SAVE_REDUCE_ALL
        } 
        barrier(CLK_LOCAL_MEM_FENCE); 
    } 
    if(lid == 0 && valid_pos(index0,limit)) {
        LOAD_REDUCED_SAVE_GLOBAL_ALL
    }
}

