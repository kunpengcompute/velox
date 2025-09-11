/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */

.text
.align        6
.arch        armv8-a+crc+crypto

.macro    crc32_u64    dst,src,data
    crc32x        \dst,\src,\data
.endm

.macro    crc32_u32    dst,src,data
    crc32w        \dst,\src,\data
.endm

.macro    crc32_u16    dst,src,data
    crc32h        \dst,\src,\data
.endm

.macro    crc32_u8    dst,src,data
    crc32b        \dst,\src,\data
.endm

.macro    declare_var_vector_reg name:req,reg:req
    q\name    .req    q\reg
    v\name    .req    v\reg
    s\name    .req    s\reg
    d\name    .req    d\reg
.endm
    BUF        .req    x0
    LEN        .req    x1
    wCRC        .req    w2
    crc0        .req    w2
    crc1        .req    w3
    crc2        .req    w4
    crc3        .req    w5
    
    xcrc0        .req    x2
    xcrc1        .req    x3
    xcrc2        .req    x4
    xcrc3        .req    x5
    
    const_adr    .req    x17
    ptr_crc0    .req    x0
    ptr_crc1    .req    x6
    ptr_crc2    .req    x7
    ptr_crc3    .req    x8

    crc0_data0    .req    x9
    crc0_data1    .req    x10
    crc0_data2    .req    x11
    crc0_data3    .req    x12
    crc0_data4    .req    x13
    crc0_data5    .req    x14

    crc0_data6    .req    x15
    crc0_data7    .req    x16
    
    wdata        .req    w9
    data0        .req    x9
    data1        .req    x10
    data2        .req    x11
    data3        .req    x12

    declare_var_vector_reg    tmp0,0
    declare_var_vector_reg    tmp1,1
    declare_var_vector_reg    tmp2,2


    declare_var_vector_reg    const0,3
    declare_var_vector_reg    const1,4
    declare_var_vector_reg    const2,5
    
/**
    unsigned int crc32_x4(
        unsigned char *BUF,
        int LEN,
        unsigned int wCRC
    );

*/

.global    crc32_x4
.type    crc32_x4, %function
crc32_x4:
    
    cbz        LEN, .zero_length_ret

.align_finish:
    cmp        LEN,16384
    blo        .block_1KB
    adrp    const_adr, .Lconstants_16KB
    add        const_adr, const_adr, #:lo12:.Lconstants_16KB
    ldp        dconst0,dconst1,[const_adr]
    ldr        dconst2, [const_adr, 16]

.loop_16KB:
    mov        crc1,0
    mov        crc2,0
    mov        crc3,0

    add        ptr_crc1,ptr_crc0,4096
    add        ptr_crc2,ptr_crc1,4096
    add        ptr_crc3,ptr_crc2,4096

    ldp        crc0_data0, crc0_data1, [ptr_crc0], 16
    ldp        crc0_data2, crc0_data3, [ptr_crc1], 16

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data4, crc0_data5, [ptr_crc2], 16
    ldp        crc0_data6, crc0_data7, [ptr_crc3], 16

    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6

.rept 255
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7

    ldr        crc0_data1, [ptr_crc0], 8
    ldr        crc0_data3, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldr        crc0_data5, [ptr_crc2], 8
    ldr        crc0_data7, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
.endr
    
    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7

    fmov        dtmp0,xcrc0
    fmov        dtmp1,xcrc1
    fmov        dtmp2,xcrc2

    mov        ptr_crc0,ptr_crc3

    sub        LEN,LEN,16384

    pmull        vtmp0.1q,vtmp0.1d,vconst0.1d
    pmull        vtmp1.1q,vtmp1.1d,vconst1.1d
    pmull        vtmp2.1q,vtmp2.1d,vconst2.1d

    cmp        LEN,16384

    fmov        xcrc0,dtmp0
    fmov        xcrc1,dtmp1
    fmov        xcrc2,dtmp2
    
    crc32_u64    crc0,wzr,xcrc0
    crc32_u64    crc1,wzr,xcrc1
    crc32_u64    crc2,wzr,xcrc2

    eor        crc0,crc0,crc1
    eor        crc2,crc2,crc3
    eor        crc0,crc0,crc2

    bhs        .loop_16KB

.block_1KB:
    cmp        LEN,1024
    blo        .block_512B
    adrp    const_adr, .Lconstants_1KB
    add        const_adr, const_adr, #:lo12:.Lconstants_1KB
    ldp        dconst0,dconst1,[const_adr]
    ldr        dconst2, [const_adr, 16]

.loop_1KB:    
    mov        crc1,0
    mov        crc2,0
    mov        crc3,0

    add        ptr_crc1,ptr_crc0,248
    add        ptr_crc2,ptr_crc0,248*2
    add        ptr_crc3,ptr_crc0,248*3

    ldp        crc0_data0, crc0_data1, [ptr_crc0], 16
    ldp        crc0_data2, crc0_data3, [ptr_crc1], 16

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data4, crc0_data5, [ptr_crc2], 16
    ldp        crc0_data6, crc0_data7, [ptr_crc3], 16

    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6

.rept 14
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7

    ldr        crc0_data1, [ptr_crc0], 8
    ldr        crc0_data3, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldr        crc0_data5, [ptr_crc2], 8
    ldr        crc0_data7, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
.endr
    
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7


    ldp        crc0_data1, crc0_data3, [ptr_crc3], 16
    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data5, crc0_data7, [ptr_crc3], 16
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
    
    crc32_u64    crc3,crc3,crc0_data1
    crc32_u64    crc3,crc3,crc0_data3

    fmov        dtmp0,xcrc0
    fmov        dtmp1,xcrc1
    fmov        dtmp2,xcrc2

    mov        ptr_crc0,ptr_crc3
    
    sub        LEN,LEN,1024
    
    crc32_u64    crc3,crc3,crc0_data5
    
    pmull        vtmp0.1q,vtmp0.1d,vconst0.1d
    pmull        vtmp1.1q,vtmp1.1d,vconst1.1d
    pmull        vtmp2.1q,vtmp2.1d,vconst2.1d


    cmp        LEN,1024

    fmov        xcrc0,dtmp0
    fmov        xcrc1,dtmp1
    fmov        xcrc2,dtmp2
    
    crc32_u64    crc0,wzr,xcrc0
    crc32_u64    crc1,wzr,xcrc1
    crc32_u64    crc2,wzr,xcrc2
    crc32_u64    crc3,crc3,crc0_data7

    eor        crc0,crc0,crc1
    eor        crc2,crc2,crc3
    eor        crc0,crc0,crc2

    bhs        .loop_1KB

.block_512B:
    cmp        LEN,512
    blo        .block_256B
    adrp    const_adr, .Lconstants_512B
    add        const_adr, const_adr, #:lo12:.Lconstants_512B
    ldp        dconst0,dconst1,[const_adr]
    ldr        dconst2, [const_adr, 16]

.loop_512B:    
    mov        crc1,0
    mov        crc2,0
    mov        crc3,0

    add        ptr_crc1,ptr_crc0,120
    add        ptr_crc2,ptr_crc0,120*2
    add        ptr_crc3,ptr_crc0,120*3

    ldp        crc0_data0, crc0_data1, [ptr_crc0], 16
    ldp        crc0_data2, crc0_data3, [ptr_crc1], 16

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data4, crc0_data5, [ptr_crc2], 16
    ldp        crc0_data6, crc0_data7, [ptr_crc3], 16

    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6

.rept 6
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7

    ldr        crc0_data1, [ptr_crc0], 8
    ldr        crc0_data3, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldr        crc0_data5, [ptr_crc2], 8
    ldr        crc0_data7, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
.endr
    
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7


    ldp        crc0_data1, crc0_data3, [ptr_crc3], 16
    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data5, crc0_data7, [ptr_crc3], 16
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
    
    crc32_u64    crc3,crc3,crc0_data1
    crc32_u64    crc3,crc3,crc0_data3

    fmov        dtmp0,xcrc0
    fmov        dtmp1,xcrc1
    fmov        dtmp2,xcrc2

    mov        ptr_crc0,ptr_crc3
    
    sub        LEN,LEN,512
    
    crc32_u64    crc3,crc3,crc0_data5
    
    pmull        vtmp0.1q,vtmp0.1d,vconst0.1d
    pmull        vtmp1.1q,vtmp1.1d,vconst1.1d
    pmull        vtmp2.1q,vtmp2.1d,vconst2.1d


    cmp        LEN,512

    fmov        xcrc0,dtmp0
    fmov        xcrc1,dtmp1
    fmov        xcrc2,dtmp2
    
    crc32_u64    crc0,wzr,xcrc0
    crc32_u64    crc1,wzr,xcrc1
    crc32_u64    crc2,wzr,xcrc2
    crc32_u64    crc3,crc3,crc0_data7

    eor        crc0,crc0,crc1
    eor        crc2,crc2,crc3
    eor        crc0,crc0,crc2

    bhs        .loop_512B

.block_256B:
    cmp        LEN,256
    blo        .block_128B
    adrp    const_adr, .Lconstants_256B
    add        const_adr, const_adr, #:lo12:.Lconstants_256B
    ldp        dconst0,dconst1,[const_adr]
    ldr        dconst2, [const_adr, 16]

.loop_256B:    
    mov        crc1,0
    mov        crc2,0
    mov        crc3,0

    add        ptr_crc1,ptr_crc0,56
    add        ptr_crc2,ptr_crc0,56*2
    add        ptr_crc3,ptr_crc0,56*3

    ldp        crc0_data0, crc0_data1, [ptr_crc0], 16
    ldp        crc0_data2, crc0_data3, [ptr_crc1], 16

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data4, crc0_data5, [ptr_crc2], 16
    ldp        crc0_data6, crc0_data7, [ptr_crc3], 16

    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6

.rept 2
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7

    ldr        crc0_data1, [ptr_crc0], 8
    ldr        crc0_data3, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldr        crc0_data5, [ptr_crc2], 8
    ldr        crc0_data7, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
.endr
    
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7


    ldp        crc0_data1, crc0_data3, [ptr_crc3], 16
    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data5, crc0_data7, [ptr_crc3], 16
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
    
    crc32_u64    crc3,crc3,crc0_data1
    crc32_u64    crc3,crc3,crc0_data3

    fmov        dtmp0,xcrc0
    fmov        dtmp1,xcrc1
    fmov        dtmp2,xcrc2

    mov        ptr_crc0,ptr_crc3
    
    sub        LEN,LEN,256
    
    crc32_u64    crc3,crc3,crc0_data5
    
    pmull        vtmp0.1q,vtmp0.1d,vconst0.1d
    pmull        vtmp1.1q,vtmp1.1d,vconst1.1d
    pmull        vtmp2.1q,vtmp2.1d,vconst2.1d


    cmp        LEN,256

    fmov        xcrc0,dtmp0
    fmov        xcrc1,dtmp1
    fmov        xcrc2,dtmp2
    
    crc32_u64    crc0,wzr,xcrc0
    crc32_u64    crc1,wzr,xcrc1
    crc32_u64    crc2,wzr,xcrc2
    crc32_u64    crc3,crc3,crc0_data7

    eor        crc0,crc0,crc1
    eor        crc2,crc2,crc3
    eor        crc0,crc0,crc2

    bhs        .loop_256B

.block_128B:
    cmp        LEN,128
    blo        .block_64B
    adrp    const_adr, .Lconstants_128B
    add        const_adr, const_adr, #:lo12:.Lconstants_128B
    ldp        dconst0,dconst1,[const_adr]
    ldr        dconst2, [const_adr, 16]

.loop_128B:    
    mov        crc1,0
    mov        crc2,0
    mov        crc3,0

    add        ptr_crc1,ptr_crc0,24
    add        ptr_crc2,ptr_crc0,24*2
    add        ptr_crc3,ptr_crc0,24*3

    ldp        crc0_data0, crc0_data1, [ptr_crc0], 16
    ldp        crc0_data2, crc0_data3, [ptr_crc1], 16

    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data4, crc0_data5, [ptr_crc2], 16
    ldp        crc0_data6, crc0_data7, [ptr_crc3], 16

    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6

    
    ldr        crc0_data0, [ptr_crc0], 8
    ldr        crc0_data2, [ptr_crc1], 8

    crc32_u64    crc0,crc0,crc0_data1
    crc32_u64    crc1,crc1,crc0_data3

    ldr        crc0_data4, [ptr_crc2], 8
    ldr        crc0_data6, [ptr_crc3], 8
    
    crc32_u64    crc2,crc2,crc0_data5
    crc32_u64    crc3,crc3,crc0_data7


    ldp        crc0_data1, crc0_data3, [ptr_crc3], 16
    crc32_u64    crc0,crc0,crc0_data0
    crc32_u64    crc1,crc1,crc0_data2

    ldp        crc0_data5, crc0_data7, [ptr_crc3], 16
    crc32_u64    crc2,crc2,crc0_data4
    crc32_u64    crc3,crc3,crc0_data6
    
    crc32_u64    crc3,crc3,crc0_data1
    crc32_u64    crc3,crc3,crc0_data3

    fmov        dtmp0,xcrc0
    fmov        dtmp1,xcrc1
    fmov        dtmp2,xcrc2

    mov        ptr_crc0,ptr_crc3
    
    sub        LEN,LEN,128
    
    crc32_u64    crc3,crc3,crc0_data5
    
    pmull        vtmp0.1q,vtmp0.1d,vconst0.1d
    pmull        vtmp1.1q,vtmp1.1d,vconst1.1d
    pmull        vtmp2.1q,vtmp2.1d,vconst2.1d


    cmp        LEN,128

    fmov        xcrc0,dtmp0
    fmov        xcrc1,dtmp1
    fmov        xcrc2,dtmp2
    
    crc32_u64    crc0,wzr,xcrc0
    crc32_u64    crc1,wzr,xcrc1
    crc32_u64    crc2,wzr,xcrc2
    crc32_u64    crc3,crc3,crc0_data7

    eor        crc0,crc0,crc1
    eor        crc2,crc2,crc3
    eor        crc0,crc0,crc2

    bhs        .loop_128B
    
.block_64B:
    cmp        LEN, 63
    bls        .loop_16B
.loop_64B:
    ldp        data0, data1, [BUF],#16
    sub        LEN,LEN,#64
    ldp        data2, data3, [BUF],#16
    cmp        LEN,#64
    crc32_u64    wCRC, wCRC, data0
    crc32_u64    wCRC, wCRC, data1
    ldp        data0, data1, [BUF],#16
    crc32_u64    wCRC, wCRC, data2
    crc32_u64    wCRC, wCRC, data3
    ldp        data2, data3, [BUF],#16
    crc32_u64    wCRC, wCRC, data0
    crc32_u64    wCRC, wCRC, data1
    crc32_u64    wCRC, wCRC, data2
    crc32_u64    wCRC, wCRC, data3
    bge    .loop_64B

.loop_16B:
    cmp        LEN, 15
    bls        .less_16B
    ldp        data0, data1, [BUF],#16
    sub        LEN,LEN,#16
    cmp        LEN,15
    crc32_u64    wCRC, wCRC, data0
    crc32_u64    wCRC, wCRC, data1
    bls        .less_16B
    ldp        data0, data1, [BUF],#16
    sub        LEN,LEN,#16
    cmp        LEN,15
    crc32_u64    wCRC, wCRC, data0
    crc32_u64    wCRC, wCRC, data1
    bls        .less_16B
    ldp        data0, data1, [BUF],#16
    sub        LEN,LEN,#16   //MUST less than 16B
    crc32_u64    wCRC, wCRC, data0
    crc32_u64    wCRC, wCRC, data1
.less_16B:
    tbz        LEN, 3, .less_8B
    ldr        data0, [BUF], 8
    crc32_u64        wCRC, wCRC, data0
.less_8B:
    tbz        LEN, 2, .less_4B
    ldr        wdata, [BUF], 4
    crc32_u32        wCRC, wCRC, wdata
.less_4B:
    tbz        LEN, 1, .less_2B
    ldrh        wdata, [BUF], 2
    crc32_u16        wCRC, wCRC, wdata
.less_2B:
    tbz        LEN, 0, .zero_length_ret
    ldrb        wdata, [BUF]
    crc32_u8        wCRC, wCRC, wdata

.zero_length_ret:
    mov w0, wCRC
    ret

.size    crc32_x4, .-crc32_x4

.section .rodata
.align 4
.Lconstants_128B:
    .quad        0xdf068dc2
    .quad        0xae0b5394
    .quad        0x3db1ecdc


.section .rodata
.align 4
.Lconstants_256B:
    .quad        0x596c8d81
    .quad        0x3f41287a
    .quad        0x1c279815


.section .rodata
.align 4
.Lconstants_512B:
    .quad        0x2ce423f1
    .quad        0xb918a347
    .quad        0x26b70c3d


.section .rodata
.align 4
.Lconstants_1KB:
    .quad        0x1423c53a
    .quad        0xa55d1514
    .quad        0xa749e894

.section .rodata
.align 4
.Lconstants_16KB:
    .quad        0x4470c029
    .quad        0x2339d155
    .quad        0x68c0a2c5

