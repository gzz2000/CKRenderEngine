// WebGPU implementation of the draw packets produced by CKFFPLib.
// This module owns GPU objects; no WebGPU handle is stored in game snapshots.
const compare = ['always','never','less','equal','less-equal','greater','not-equal','greater-equal','always'];
const stencil = ['keep','keep','zero','replace','increment-clamp','decrement-clamp','invert','increment-wrap','decrement-wrap'];
const blend = ['one','zero','one','src','one-minus-src','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha','dst','one-minus-dst','src-alpha-saturated'];
const op = ['add','add','subtract','reverse-subtract','min','max'];
const address = ['repeat','repeat','mirror-repeat','clamp-to-edge','clamp-to-edge'];
const topology = ['triangle-list','point-list','line-list','line-strip','triangle-list','triangle-strip','triangle-list'];

export async function createBallanceGpu(canvas, shaderSources) {
    if (!navigator.gpu) throw new Error('WebGPU is unavailable');
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('WebGPU adapter is unavailable');
    const device = await adapter.requestDevice();
    const context = canvas.getContext('webgpu');
    const format = navigator.gpu.getPreferredCanvasFormat();
    context.configure({device, format, alphaMode:'opaque'});
    const errors = [];
    device.addEventListener('uncapturederror', event => {
        errors.push(event.error.message);
        console.error('Ballance WebGPU:',event.error.message);
    });
    const modules = {};
    for (const [name, code] of Object.entries(shaderSources)) {
        // Clip-distance shaders are an optional device feature, not needed by
        // this context until its advertised clip-plane support is enabled.
        if (name.includes('_clip')) continue;
        const module = device.createShaderModule({label:name,code});
        const info = await module.getCompilationInfo();
        const failures = info.messages.filter(m=>m.type==='error');
        if (failures.length) throw new Error(`${name}: ${failures.map(m=>m.message).join('\n')}`);
        modules[name] = module;
    }
    return new BallanceGpu(device,context,format,canvas,modules,errors);
}

class BallanceGpu {
    constructor(device,context,format,canvas,modules,errors) {
        Object.assign(this,{device,context,format,canvas,modules,errors});
        this.textures=new Map();this.samplers=new Map();this.pipelines=new Map();
        this.samples=1;this.uploadBlocks=[];this.uploadIndex=0;this.retiredTextures=[];this.frames=0;this.draws=0;this.defaultTextures=[];
        for (let kind=0;kind<3;++kind) {
            const id=-kind-1;this.texture(id,1,1,1,1,kind);
            for(let face=0;face<(kind===1?6:1);++face)this.upload(id,0,face,1,1,0,0,new Uint8Array([255,255,255,255]));
            this.defaultTextures[kind]=this.textures.get(id);
        }
    }
    setSamples(requested) {
        const samples=requested>1?4:1;
        if (samples===this.samples) return true;
        this.present();
        this.samples=samples;
        if (this.depth) {
            this.depth.destroy();this.depth=null;
            return this.resize(this.logicalWidth,this.logicalHeight,this.canvas.width,this.canvas.height);
        }
        return true;
    }
    resize(logicalWidth,logicalHeight,width=logicalWidth,height=logicalHeight) {
        if (!(width>0&&height>0)) return false;
        this.logicalWidth=logicalWidth;this.logicalHeight=logicalHeight;
        this.canvas.style.width=`${logicalWidth}px`;
        this.canvas.style.height=`${logicalHeight}px`;
        if(this.targetWidth===width&&this.targetHeight===height&&this.depth)return true;
        this.present();
        this.canvas.style.width=`${logicalWidth}px`;
        this.canvas.style.height=`${logicalHeight}px`;
        this.canvas.width=width;this.canvas.height=height;
        this.targetWidth=width;this.targetHeight=height;
        this.depth?.destroy();
        this.depth=this.device.createTexture({label:'Ballance depth',size:[width,height],sampleCount:this.samples,format:'depth24plus-stencil8',usage:GPUTextureUsage.RENDER_ATTACHMENT});
        this.multisample?.destroy();
        this.multisample=this.samples>1?this.device.createTexture({label:'Ballance MSAA',
            size:[width,height],sampleCount:this.samples,format:this.format,usage:GPUTextureUsage.RENDER_ATTACHMENT}):null;
        return true;
    }
    beginPass(clear) {
        if(this.pass) return;
        if(!this.encoder)this.encoder=this.device.createCommandEncoder();
        if(!this.surface)this.surface=this.context.getCurrentTexture();
        const flags=clear?.flags??0;
        this.pass=this.encoder.beginRenderPass({
            colorAttachments:[{view:(this.multisample??this.surface).createView(),resolveTarget:this.multisample?this.surface.createView():undefined,loadOp:flags&32?'clear':'load',storeOp:'store',clearValue:clear?.color??[0,0,0,1]}],
            depthStencilAttachment:{view:this.depth.createView(),depthLoadOp:flags&16?'clear':'load',depthStoreOp:'store',depthClearValue:clear?.depth??1,
                stencilLoadOp:flags&64?'clear':'load',stencilStoreOp:'store',stencilClearValue:clear?.stencil??0},
        });
    }
    clear(flags,color,depth,stencil) {
        if(this.pass){this.pass.end();this.pass=null;}
        this.beginPass({flags,color:[((color>>>16)&255)/255,((color>>>8)&255)/255,(color&255)/255,(color>>>24)/255],depth,stencil});
    }
    clearRect(flags,color,depth,stencil,rect) {
        const sx=this.canvas.width/this.logicalWidth,sy=this.canvas.height/this.logicalHeight;
        const left=Math.max(0,Math.floor(rect[0]*sx)),top=Math.max(0,Math.floor(rect[1]*sy));
        const right=Math.min(this.canvas.width,Math.ceil(rect[2]*sx)),bottom=Math.min(this.canvas.height,Math.ceil(rect[3]*sy));
        if(right<=left||bottom<=top||!(flags&112))return;
        if(left===0&&top===0&&right===this.canvas.width&&bottom===this.canvas.height){this.clear(flags,color,depth,stencil);return;}
        this.beginPass();
        this.clearModule??=this.device.createShaderModule({label:'Scissored clear',code:`
            struct Values {color:vec4f,depth:vec4f};
            @group(0) @binding(0) var<uniform> values:Values;
            @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                let p=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
                return vec4f(p[i],0,1);
            }
            struct Pixel {@location(0) color:vec4f,@builtin(frag_depth) depth:f32};
            @fragment fn fs()->Pixel {return Pixel(values.color,values.depth.x);}
        `});
        this.clearPipelines??=new Map();const key=(flags&112)+':'+this.samples;
        let pipeline=this.clearPipelines.get(key);
        if(!pipeline){
            const face={compare:'always',failOp:'replace',depthFailOp:'replace',passOp:'replace'};
            pipeline=this.device.createRenderPipeline({layout:'auto',
                vertex:{module:this.clearModule,entryPoint:'vs'},
                fragment:{module:this.clearModule,entryPoint:'fs',targets:[{format:this.format,writeMask:flags&32?15:0}]},
                depthStencil:{format:'depth24plus-stencil8',depthCompare:'always',depthWriteEnabled:!!(flags&16),
                    stencilFront:face,stencilBack:face,stencilReadMask:255,stencilWriteMask:flags&64?255:0},
                multisample:{count:this.samples}});
            this.clearPipelines.set(key,pipeline);
        }
        const values=new Float32Array([((color>>>16)&255)/255,((color>>>8)&255)/255,(color&255)/255,(color>>>24)/255,depth,0,0,0]);
        const buffer=this.buffer(new Uint8Array(values.buffer),GPUBufferUsage.UNIFORM);
        this.pass.setPipeline(pipeline);
        this.pass.setBindGroup(0,this.device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:0,resource:buffer}]}));
        this.pass.setScissorRect(left,top,right-left,bottom-top);this.pass.setStencilReference(stencil&255);this.pass.draw(3);
    }
    flush() {
        if(this.pass){this.pass.end();this.pass=null;}
        if(this.encoder){
            // One upload per arena instead of hundreds of tiny per-draw writes.
            // Writes precede this submission and follow the previous one on the
            // same queue, so arena reuse needs no GPU wait or CPU spinning.
            for(const block of this.uploadBlocks)if(block.used)this.device.queue.writeBuffer(block.gpu,0,block.bytes,0,block.used);
            this.device.queue.submit([this.encoder.finish()]);this.encoder=null;
        }
        const used=this.uploadIndex+1;
        for(const block of this.uploadBlocks)block.used=0;
        // Keep normal frame capacity, retire rare oversized-frame spare blocks.
        while(this.uploadBlocks.length>used&&this.uploadBlocks.reduce((n,b)=>n+b.bytes.length,0)>64*1024*1024)this.uploadBlocks.pop().gpu.destroy();
        this.uploadIndex=0;
        for(const texture of this.retiredTextures)texture.destroy();this.retiredTextures=[];
    }
    present() {
        if(this.surface)++this.frames;
        this.flush();this.surface=null;
    }
    texture(id,width,height,depth,levels,kind) {
        const texture=this.device.createTexture({label:`Ballance texture ${id}`,size:[width,height,kind===1?6:kind===2?depth:1],
            dimension:kind===2?'3d':'2d',format:'rgba8unorm',mipLevelCount:levels,
            usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST|GPUTextureUsage.COPY_SRC|GPUTextureUsage.RENDER_ATTACHMENT});
        this.textures.set(id,{texture,view:texture.createView({dimension:['2d','cube','3d'][kind]}),width,height,depth,levels,kind});
        return true;
    }
    upload(id,mip,face,width,height,x,y,data) {
        const record=this.textures.get(id);if(!record)return false;
        // Queue writes must follow earlier encoded draws that read this image.
        // Preserve the same surface/pass contents while submitting that prefix.
        if(this.encoder)this.flush();
        this.device.queue.writeTexture({texture:record.texture,mipLevel:mip,origin:[x,y,face]},data,{bytesPerRow:width*4,rowsPerImage:height},[width,height,1]);
        // Engine may supply each mip explicitly. Until then fill the mip chain
        // from base pixels, so an allocated but uninitialized mip never samples black.
        if(mip===0&&x===0&&y===0&&record.kind!==2&&width===record.width&&height===record.height) {
            let previous=data;
            for(let level=1;level<record.levels;++level){
                const w=Math.max(1,width>>1),h=Math.max(1,height>>1),next=new Uint8Array(w*h*4);
                for(let row=0;row<h;++row)for(let col=0;col<w;++col)for(let c=0;c<4;++c){
                    let sum=0;for(let dy=0;dy<2;++dy)for(let dx=0;dx<2;++dx)sum+=previous[(Math.min(height-1,row*2+dy)*width+Math.min(width-1,col*2+dx))*4+c];
                    next[(row*w+col)*4+c]=Math.round(sum/4);
                }
                this.device.queue.writeTexture({texture:record.texture,mipLevel:level,origin:[0,0,face]},next,{bytesPerRow:w*4,rowsPerImage:h},[w,h,1]);
                previous=next;width=w;height=h;
            }
        }
        return true;
    }
    deleteTexture(id){const record=this.textures.get(id);if(record){this.retiredTextures.push(record.texture);this.textures.delete(id);}}
    buffer(bytes,usage){
        const alignment=usage===GPUBufferUsage.UNIFORM?this.device.limits.minUniformBufferOffsetAlignment:4;
        const size=Math.max(4,(bytes.byteLength+3)&~3);
        let block,offset;
        for(;;){
            block=this.uploadBlocks[this.uploadIndex];
            if(!block){
                const capacity=Math.max(4*1024*1024,2**Math.ceil(Math.log2(size)));
                block={bytes:new Uint8Array(capacity),used:0,gpu:this.device.createBuffer({size:capacity,
                    usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.UNIFORM|GPUBufferUsage.VERTEX|GPUBufferUsage.INDEX})};
                this.uploadBlocks.push(block);
            }
            offset=Math.ceil(block.used/alignment)*alignment;
            if(offset+size<=block.bytes.length)break;
            ++this.uploadIndex;
        }
        block.bytes.set(bytes,offset);block.used=offset+size;
        return {buffer:block.gpu,offset,size:bytes.byteLength};
    }
    beginDraw(state,positionT,layout,vertices,count,indices,indexCount,index32){
        this.pending={state,positionT,layout,vertices,count,indices,indexCount,index32,uniforms:[],textures:[]};
    }
    uniform(group,binding,bytes){this.pending.uniforms.push({group,binding,resource:this.buffer(bytes,GPUBufferUsage.UNIFORM)});}
    binding(slot,id,s,dimension){
        const texture=this.textures.get(id)??this.defaultTextures[dimension];
        const key=JSON.stringify(s.slice(0,10));let sampler=this.samplers.get(key);
        if(!sampler){
            const minFilter=s[0]===1?'nearest':'linear',magFilter=s[1]===1?'nearest':'linear';
            sampler=this.device.createSampler({addressModeU:address[s[3]],addressModeV:address[s[4]],addressModeW:address[s[5]],
                minFilter,magFilter,mipmapFilter:s[2]===2?'linear':'nearest',lodMinClamp:s[8],lodMaxClamp:s[2]===0?s[8]:32});
            this.samplers.set(key,sampler);
        }
        this.pending.textures.push({binding:slot*2,resource:texture.view},{binding:slot*2+1,resource:sampler});
    }
    pipeline(draw){
        const [lo,mid,hi,,readMask,writeMask]=draw.state;
        const key=JSON.stringify([draw.positionT,draw.layout,lo,mid,hi,readMask,writeMask,!!draw.indexCount,!!draw.index32,this.samples]);
        let pipeline=this.pipelines.get(key);if(pipeline)return pipeline;
        const vertex=this.modules[draw.positionT?'vs_ff_positiont':'vs_ff_3d'];
        const fragment=this.modules[['fs_ff_stage','fs_ff_stage_cube','fs_ff_stage_volume'][draw.layout]];
        const attributes=Array.from({length:16},(_,i)=>({shaderLocation:i,offset:i*16,format:i===6?'uint32x4':([1,2,3,7].includes(i)?'float32x3':'float32x4')}));
        const cull=(lo>>>10)&3,src=(lo>>>16)&15,dst=(lo>>>20)&15,srcA=(lo>>>24)&15,dstA=lo>>>28;
        const factor=(n,alpha)=>alpha&&n===3?'src-alpha':alpha&&n===4?'one-minus-src-alpha':alpha&&n===9?'dst-alpha':alpha&&n===10?'one-minus-dst-alpha':alpha&&n===11?'one':blend[n];
        const colorBlend=src?{color:{srcFactor:factor(src,false),dstFactor:factor(dst,false),operation:op[mid&7]},
            alpha:{srcFactor:factor(srcA||src,true),dstFactor:factor(dstA||dst,true),operation:op[(mid>>>3)&7]}}:undefined;
        const stencilFace=bits=>({compare:compare[bits&15],failOp:stencil[(bits>>>4)&15],depthFailOp:stencil[(bits>>>8)&15],passOp:stencil[(bits>>>12)&15]});
        const primitive={topology:topology[(mid>>>6)&7],cullMode:['none','front','back','none'][cull],frontFace:hi&0x10000?'ccw':'cw'};
        if(primitive.topology.endsWith('strip')&&draw.indexCount)primitive.stripIndexFormat=draw.index32?'uint32':'uint16';
        const ds={format:'depth24plus-stencil8',depthWriteEnabled:!!(lo&32),depthCompare:lo&16?compare[(lo>>>6)&15]:'always'};
        if(mid&0x200){ds.stencilFront=stencilFace(mid>>>10);ds.stencilBack=(hi&65535)?stencilFace(hi):ds.stencilFront;ds.stencilReadMask=readMask&255;ds.stencilWriteMask=writeMask&255;}
        pipeline=this.device.createRenderPipeline({label:'Ballance fixed function',layout:'auto',
            vertex:{module:vertex,entryPoint:'main',buffers:[{arrayStride:256,attributes}]},
            fragment:{module:fragment,entryPoint:'main',targets:[{format:this.format,blend:colorBlend,writeMask:lo&15}]},primitive,depthStencil:ds,multisample:{count:this.samples}});
        this.pipelines.set(key,pipeline);return pipeline;
    }
    endDraw(){
        const draw=this.pending;this.pending=null;
        this.beginPass();const pass=this.pass,pipeline=this.pipeline(draw);
        pass.setPipeline(pipeline);
        for(const group of [1,3])pass.setBindGroup(group,this.device.createBindGroup({layout:pipeline.getBindGroupLayout(group),entries:
            draw.uniforms.filter(u=>u.group===group).map(u=>({binding:u.binding,resource:u.resource}))}));
        pass.setBindGroup(2,this.device.createBindGroup({layout:pipeline.getBindGroupLayout(2),entries:draw.textures}));
        const vertex=this.buffer(draw.vertices,GPUBufferUsage.VERTEX);pass.setVertexBuffer(0,vertex.buffer,vertex.offset,vertex.size);
        const s=draw.state;
        const left=s[6]?Math.max(0,s[7]):0,top=s[6]?Math.max(0,s[8]):0;
        const right=s[6]?Math.min(this.canvas.width,s[9]):this.canvas.width,bottom=s[6]?Math.min(this.canvas.height,s[10]):this.canvas.height;
        if(right<=left||bottom<=top)return true;
        pass.setScissorRect(left,top,right-left,bottom-top);pass.setStencilReference(s[3]&255);
        if(draw.indexCount){const index=this.buffer(draw.indices,GPUBufferUsage.INDEX);pass.setIndexBuffer(index.buffer,draw.index32?'uint32':'uint16',index.offset,index.size);pass.drawIndexed(draw.indexCount);}
        else pass.draw(draw.count);
        ++this.draws;return true;
    }
    shutdown(){this.present();for(const block of this.uploadBlocks)block.gpu.destroy();this.uploadBlocks=[];for(const r of this.textures.values())r.texture.destroy();this.textures.clear();this.depth?.destroy();this.multisample?.destroy();}
}
