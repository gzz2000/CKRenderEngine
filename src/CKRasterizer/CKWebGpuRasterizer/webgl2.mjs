// WebGL2 implementation of the same CKFFPLib draw-packet ABI as webgpu.mjs.
// Only graphics resources live here; simulation/snapshots do not depend on GL.
export async function createBallanceGl(canvas,sources) {
    const gl=canvas.getContext('webgl2',{alpha:false,antialias:false,depth:false,stencil:false,preserveDrawingBuffer:false});
    if(!gl)throw new Error('WebGL2 is unavailable (check browser graphics acceleration and GPU drivers)');
    return new BallanceGl(gl,canvas,sources);
}
class BallanceGl {
    constructor(gl,canvas,sources){
        Object.assign(this,{gl,canvas,backend:'webgl2',errors:[],frames:0,draws:0,samples:1});
        this.textures=new Map();this.samplers=new Map();this.programs=new Map();this.ubos=new Map();this.shaders={};
        this.vbo=gl.createBuffer();this.ibo=gl.createBuffer();this.vao=gl.createVertexArray();gl.bindVertexArray(this.vao);
        this.onLost=e=>{e.preventDefault();this.errors.push('WebGL context lost; reload to restore the game');};
        canvas.addEventListener('webglcontextlost',this.onLost);
        try{
            for(const [name,source] of Object.entries(sources)){
                const shader=gl.createShader(name.startsWith('vs_')?gl.VERTEX_SHADER:gl.FRAGMENT_SHADER);
                gl.shaderSource(shader,source);gl.compileShader(shader);
                if(!gl.getShaderParameter(shader,gl.COMPILE_STATUS)){const error=gl.getShaderInfoLog(shader);gl.deleteShader(shader);throw new Error(name+': '+error);}
                this.shaders[name]=shader;
            }
            // Link all six FFP layouts now, so unsupported drivers fail before play.
            for(let pt=0;pt<2;pt++)for(let layout=0;layout<3;layout++)this.program(pt,layout);
            this.defaultTextures=[];
            for(let kind=0;kind<3;kind++){
                const id=-kind-1;this.texture(id,1,1,1,1,kind);
                for(let face=0;face<(kind===1?6:1);face++)this.upload(id,0,face,1,1,0,0,new Uint8Array([255,255,255,255]));
                this.defaultTextures[kind]=this.textures.get(id);
            }
            this.blitProgram=gl.createProgram();this.blitVao=gl.createVertexArray();
            for(const [type,code] of [[gl.VERTEX_SHADER,`#version 300 es
                out vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}`],
                [gl.FRAGMENT_SHADER,`#version 300 es
                precision highp float;uniform sampler2D image;in vec2 uv;out vec4 color;void main(){color=vec4(texture(image,uv).rgb,1);}`]]){
                const shader=gl.createShader(type);gl.shaderSource(shader,code);gl.compileShader(shader);gl.attachShader(this.blitProgram,shader);gl.deleteShader(shader);
            }
            gl.linkProgram(this.blitProgram);if(!gl.getProgramParameter(this.blitProgram,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(this.blitProgram));
            this.checkError('initialization');
        }catch(error){this.shutdown();throw error;}
    }
    checkError(label){const code=this.gl.getError();if(code!==this.gl.NO_ERROR){const message=`WebGL2 ${label}: 0x${code.toString(16)}`;if(this.errors.at(-1)!==message)this.errors.push(message);return false;}return true;}
    program(positionT,layout){
        const key=Number(!!positionT)+':'+layout;if(this.programs.has(key))return this.programs.get(key);
        const gl=this.gl,p=gl.createProgram();
        gl.attachShader(p,this.shaders[positionT?'vs_ff_positiont':'vs_ff_3d']);
        gl.attachShader(p,this.shaders[['fs_ff_stage','fs_ff_stage_cube','fs_ff_stage_volume'][layout]]);gl.linkProgram(p);
        if(!gl.getProgramParameter(p,gl.LINK_STATUS)){const error=gl.getProgramInfoLog(p);gl.deleteProgram(p);throw new Error('WebGL2 FFP link: '+error);}
        gl.useProgram(p);
        for(const [group,prefix,count] of [[1,'CKVertex',3],[3,'CKFragment',2]])for(let binding=0;binding<count;binding++){
            const index=gl.getUniformBlockIndex(p,'type_'+prefix+binding);
            if(index!==gl.INVALID_INDEX)gl.uniformBlockBinding(p,index,(group===1?0:3)+binding);
        }
        for(let slot=0;slot<16;slot++)gl.uniform1i(gl.getUniformLocation(p,'ff_texture'+slot),slot);
        const record={p,mips:gl.getUniformLocation(p,'ckMipLevels[0]')};this.programs.set(key,record);return record;
    }
    setSamples(requested){
        const gl=this.gl,supported=gl.getInternalformatParameter(gl.RENDERBUFFER,gl.RGBA8,gl.SAMPLES),depth=gl.getInternalformatParameter(gl.RENDERBUFFER,gl.DEPTH24_STENCIL8,gl.SAMPLES);
        const samples=requested>1?Math.max(1,...Array.from(supported).filter(n=>n<=4&&depth.includes(n))):1;
        if(samples===this.samples)return true;
        this.samples=samples;this.targetWidth=0;
        return this.width?this.resize(this.logicalWidth,this.logicalHeight,this.width,this.height):true;
    }
    resize(logicalWidth,logicalHeight,width=logicalWidth,height=logicalHeight){
        if(!(width>0&&height>0))return false;
        const gl=this.gl;Object.assign(this,{logicalWidth,logicalHeight,width,height});
        this.canvas.style.width=logicalWidth+'px';this.canvas.style.height=logicalHeight+'px';
        if(this.targetWidth===width&&this.targetHeight===height&&this.fbo)return true;
        this.canvas.width=width;this.canvas.height=height;this.targetWidth=width;this.targetHeight=height;
        gl.deleteFramebuffer(this.fbo??null);gl.deleteRenderbuffer(this.color??null);gl.deleteRenderbuffer(this.depth??null);
        this.fbo=gl.createFramebuffer();gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);
        for(const [name,format,attachment] of [['color',gl.RGBA8,gl.COLOR_ATTACHMENT0],['depth',gl.DEPTH24_STENCIL8,gl.DEPTH_STENCIL_ATTACHMENT]]){
            this[name]=gl.createRenderbuffer();gl.bindRenderbuffer(gl.RENDERBUFFER,this[name]);
            gl.renderbufferStorageMultisample(gl.RENDERBUFFER,this.samples>1?this.samples:0,format,width,height);
            gl.framebufferRenderbuffer(gl.FRAMEBUFFER,attachment,gl.RENDERBUFFER,this[name]);
        }
        gl.deleteFramebuffer(this.resolveFbo??null);gl.deleteTexture(this.resolveTexture??null);
        this.resolveFbo=gl.createFramebuffer();this.resolveTexture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,this.resolveTexture);
        gl.texStorage2D(gl.TEXTURE_2D,1,gl.RGBA8,width,height);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.resolveFbo);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,this.resolveTexture,0);
        if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error('WebGL2 resolve framebuffer incomplete');
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);
        if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error('WebGL2 framebuffer incomplete');
        gl.viewport(0,0,width,height);return this.checkError('resize');
    }
    clear(flags,color,depth,stencil){this.clearRect(flags,color,depth,stencil,[0,0,this.logicalWidth,this.logicalHeight]);}
    clearRect(flags,color,depth,stencil,rect){
        const gl=this.gl,sx=this.width/this.logicalWidth,sy=this.height/this.logicalHeight;
        const l=Math.max(0,Math.floor(rect[0]*sx)),t=Math.max(0,Math.floor(rect[1]*sy)),r=Math.min(this.width,Math.ceil(rect[2]*sx)),b=Math.min(this.height,Math.ceil(rect[3]*sy));
        if(r<=l||b<=t)return;
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);gl.enable(gl.SCISSOR_TEST);gl.scissor(l,this.height-b,r-l,b-t);
        gl.colorMask(true,true,true,true);gl.depthMask(true);gl.stencilMask(255);
        gl.clearColor(((color>>>16)&255)/255,((color>>>8)&255)/255,(color&255)/255,(color>>>24)/255);gl.clearDepth(depth);gl.clearStencil(stencil);
        gl.clear((flags&32?gl.COLOR_BUFFER_BIT:0)|(flags&16?gl.DEPTH_BUFFER_BIT:0)|(flags&64?gl.STENCIL_BUFFER_BIT:0));this.dirty=true;
    }
    flush(){this.gl.flush();}
    present(){
        if(!this.dirty||!this.fbo)return;
        const gl=this.gl;gl.disable(gl.SCISSOR_TEST);gl.bindFramebuffer(gl.READ_FRAMEBUFFER,this.fbo);gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,this.resolveFbo);
        gl.blitFramebuffer(0,0,this.width,this.height,0,0,this.width,this.height,gl.COLOR_BUFFER_BIT,gl.NEAREST);
        // Resolve to matching RGBA8 first: an opaque default framebuffer can be
        // RGB8, which WebGL forbids as a direct multisample blit destination.
        gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.disable(gl.DEPTH_TEST);gl.disable(gl.STENCIL_TEST);gl.disable(gl.CULL_FACE);gl.disable(gl.BLEND);gl.colorMask(true,true,true,true);
        gl.useProgram(this.blitProgram);gl.bindVertexArray(this.blitVao);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,this.resolveTexture);gl.bindSampler(0,null);gl.drawArrays(gl.TRIANGLES,0,3);
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);this.dirty=false;++this.frames;
        // Once per frame, not a synchronous driver query for every draw.
        this.checkError('frame');
    }
    texture(id,width,height,depth,levels,kind){
        const gl=this.gl;this.deleteTexture(id);const texture=gl.createTexture(),target=[gl.TEXTURE_2D,gl.TEXTURE_CUBE_MAP,gl.TEXTURE_3D][kind];
        gl.bindTexture(target,texture);
        if(kind===2)gl.texStorage3D(target,levels,gl.RGBA8,width,height,depth);else gl.texStorage2D(target,levels,gl.RGBA8,width,height);
        this.textures.set(id,{texture,target,width,height,depth,levels,kind});return true;
    }
    upload(id,mip,face,width,height,x,y,data){
        const r=this.textures.get(id);if(!r)return false;const gl=this.gl;gl.bindTexture(r.target,r.texture);gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
        const upload=(level,w,h,px,py,bytes)=>{
            if(r.kind===2)gl.texSubImage3D(r.target,level,px,py,face,w,h,1,gl.RGBA,gl.UNSIGNED_BYTE,bytes);
            else gl.texSubImage2D(r.kind===1?gl.TEXTURE_CUBE_MAP_POSITIVE_X+face:r.target,level,px,py,w,h,gl.RGBA,gl.UNSIGNED_BYTE,bytes);
        };
        upload(mip,width,height,x,y,data);
        // Match WebGPU's CPU mip filter, including rounding and odd dimensions.
        if(mip===0&&x===0&&y===0&&r.kind!==2&&width===r.width&&height===r.height){
            let previous=data;
            for(let level=1;level<r.levels;level++){
                const w=Math.max(1,width>>1),h=Math.max(1,height>>1),next=new Uint8Array(w*h*4);
                for(let row=0;row<h;row++)for(let col=0;col<w;col++)for(let c=0;c<4;c++){
                    let sum=0;for(let dy=0;dy<2;dy++)for(let dx=0;dx<2;dx++)sum+=previous[(Math.min(height-1,row*2+dy)*width+Math.min(width-1,col*2+dx))*4+c];next[(row*w+col)*4+c]=Math.round(sum/4);
                }
                upload(level,w,h,0,0,next);previous=next;width=w;height=h;
            }
        }
        return true;
    }
    deleteTexture(id){const r=this.textures.get(id);if(r){this.gl.deleteTexture(r.texture);this.textures.delete(id);}}
    beginDraw(state,positionT,layout,vertices,count,indices,indexCount,index32,attributeMask=65535,vertexStride=256,defaults=null){
        const gl=this.gl;gl.bindVertexArray(this.vao);gl.bindBuffer(gl.ARRAY_BUFFER,this.vbo);gl.bufferData(gl.ARRAY_BUFFER,vertices,gl.STREAM_DRAW);
        let offset=0;
        for(let i=0;i<16;i++){
            if(attributeMask&(1<<i)){
                gl.enableVertexAttribArray(i);
                if(i===6)gl.vertexAttribIPointer(i,4,gl.UNSIGNED_INT,vertexStride,offset);
                else gl.vertexAttribPointer(i,[1,2,3,7].includes(i)?3:4,gl.FLOAT,false,vertexStride,offset);
                offset+=16;
            }else{
                gl.disableVertexAttribArray(i);
                if(i===6)gl.vertexAttribI4uiv(i,new Uint32Array(defaults.buffer,defaults.byteOffset+i*16,4));
                else gl.vertexAttrib4fv(i,new Float32Array(defaults.buffer,defaults.byteOffset+i*16,4));
            }
        }
        if(indexCount){gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,this.ibo);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,indices,gl.STREAM_DRAW);}
        const program=this.program(positionT,layout);gl.useProgram(program.p);
        this.pending={state:[...state],positionT,layout,count,indexCount,index32,program,mips:new Int32Array(16).fill(1)};
    }
    uniform(group,binding,bytes){
        const gl=this.gl,slot=(group===1?0:3)+binding;let buffer=this.ubos.get(slot);
        if(!buffer){buffer=gl.createBuffer();this.ubos.set(slot,buffer);}
        gl.bindBuffer(gl.UNIFORM_BUFFER,buffer);gl.bufferData(gl.UNIFORM_BUFFER,bytes,gl.STREAM_DRAW);gl.bindBufferBase(gl.UNIFORM_BUFFER,slot,buffer);
    }
    binding(slot,id,s,dimension){
        const gl=this.gl,r=this.textures.get(id)??this.defaultTextures[dimension],key=JSON.stringify(s.slice(0,10));let sampler=this.samplers.get(key);
        if(!sampler){
            sampler=gl.createSampler();const nearest=s[0]===1;
            gl.samplerParameteri(sampler,gl.TEXTURE_MIN_FILTER,s[2]===2?(nearest?gl.NEAREST_MIPMAP_LINEAR:gl.LINEAR_MIPMAP_LINEAR):(nearest?gl.NEAREST_MIPMAP_NEAREST:gl.LINEAR_MIPMAP_NEAREST));
            gl.samplerParameteri(sampler,gl.TEXTURE_MAG_FILTER,s[1]===1?gl.NEAREST:gl.LINEAR);
            const wrap=[gl.REPEAT,gl.REPEAT,gl.MIRRORED_REPEAT,gl.CLAMP_TO_EDGE,gl.CLAMP_TO_EDGE];
            for(const [parameter,value] of [[gl.TEXTURE_WRAP_S,s[3]],[gl.TEXTURE_WRAP_T,s[4]],[gl.TEXTURE_WRAP_R,s[5]]])gl.samplerParameteri(sampler,parameter,wrap[value]??gl.REPEAT);
            gl.samplerParameterf(sampler,gl.TEXTURE_MIN_LOD,s[8]);gl.samplerParameterf(sampler,gl.TEXTURE_MAX_LOD,s[2]===0?s[8]:32);this.samplers.set(key,sampler);
        }
        gl.activeTexture(gl.TEXTURE0+slot);gl.bindTexture(r.target,r.texture);gl.bindSampler(slot,sampler);this.pending.mips[slot]=r.levels;
    }
    endDraw(){
        const d=this.pending;this.pending=null;const gl=this.gl,s=d.state,[lo,mid,hi,ref,read,write]=s;
        gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);gl.viewport(0,0,this.width,this.height);
        const l=s[6]?Math.max(0,s[7]):0,t=s[6]?Math.max(0,s[8]):0,r=s[6]?Math.min(this.width,s[9]):this.width,b=s[6]?Math.min(this.height,s[10]):this.height;
        if(r<=l||b<=t)return true;
        gl.enable(gl.SCISSOR_TEST);gl.scissor(l,this.height-b,r-l,b-t);
        const toggle=(cap,on)=>on?gl.enable(cap):gl.disable(cap);
        const compare=[gl.ALWAYS,gl.NEVER,gl.LESS,gl.EQUAL,gl.LEQUAL,gl.GREATER,gl.NOTEQUAL,gl.GEQUAL,gl.ALWAYS];
        // ALWAYS depth test still writes depth, matching WebGPU when disabled.
        gl.enable(gl.DEPTH_TEST);gl.depthFunc(lo&16?compare[(lo>>>6)&15]:gl.ALWAYS);gl.depthMask(!!(lo&32));
        gl.colorMask(!!(lo&1),!!(lo&2),!!(lo&4),!!(lo&8));
        const cull=(lo>>>10)&3;toggle(gl.CULL_FACE,cull===1||cull===2);gl.cullFace(cull===1?gl.FRONT:gl.BACK);gl.frontFace(hi&0x10000?gl.CCW:gl.CW);
        const src=(lo>>>16)&15,dst=(lo>>>20)&15,srcA=(lo>>>24)&15,dstA=lo>>>28;
        toggle(gl.BLEND,!!src);
        if(src){
            const factors=[gl.ONE,gl.ZERO,gl.ONE,gl.SRC_COLOR,gl.ONE_MINUS_SRC_COLOR,gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA,gl.DST_ALPHA,gl.ONE_MINUS_DST_ALPHA,gl.DST_COLOR,gl.ONE_MINUS_DST_COLOR,gl.SRC_ALPHA_SATURATE];
            const factor=(n,alpha)=>factors[alpha?({3:5,4:6,9:7,10:8,11:2}[n]??n):n];
            const op=[gl.FUNC_ADD,gl.FUNC_ADD,gl.FUNC_SUBTRACT,gl.FUNC_REVERSE_SUBTRACT,gl.MIN,gl.MAX];
            gl.blendFuncSeparate(factor(src,false),factor(dst,false),factor(srcA||src,true),factor(dstA||dst,true));gl.blendEquationSeparate(op[mid&7],op[(mid>>>3)&7]);
        }
        toggle(gl.STENCIL_TEST,!!(mid&0x200));
        if(mid&0x200){
            const ops=[gl.KEEP,gl.KEEP,gl.ZERO,gl.REPLACE,gl.INCR,gl.DECR,gl.INVERT,gl.INCR_WRAP,gl.DECR_WRAP];
            for(const [face,bits] of [[gl.FRONT,mid>>>10],[gl.BACK,(hi&65535)?hi:mid>>>10]]){gl.stencilFuncSeparate(face,compare[bits&15],ref&255,read&255);gl.stencilOpSeparate(face,ops[(bits>>>4)&15],ops[(bits>>>8)&15],ops[(bits>>>12)&15]);}
            gl.stencilMask(write&255);
        }
        gl.uniform1iv(d.program.mips,d.mips);
        const mode=[gl.TRIANGLES,gl.POINTS,gl.LINES,gl.LINE_STRIP,gl.TRIANGLES,gl.TRIANGLE_STRIP,gl.TRIANGLES][(mid>>>6)&7];
        if(d.indexCount)gl.drawElements(mode,d.indexCount,d.index32?gl.UNSIGNED_INT:gl.UNSIGNED_SHORT,0);else gl.drawArrays(mode,0,d.count);
        this.dirty=true;++this.draws;return true;
    }
    shutdown(){
        const gl=this.gl;for(const r of this.textures.values())gl.deleteTexture(r.texture);for(const s of this.samplers.values())gl.deleteSampler(s);for(const {p} of this.programs.values())gl.deleteProgram(p);for(const s of Object.values(this.shaders))gl.deleteShader(s);for(const b of this.ubos.values())gl.deleteBuffer(b);
        gl.deleteBuffer(this.vbo);gl.deleteBuffer(this.ibo);gl.deleteVertexArray(this.vao);gl.deleteFramebuffer(this.fbo??null);gl.deleteRenderbuffer(this.color??null);gl.deleteRenderbuffer(this.depth??null);
        gl.deleteProgram(this.blitProgram??null);gl.deleteVertexArray(this.blitVao??null);gl.deleteFramebuffer(this.resolveFbo??null);gl.deleteTexture(this.resolveTexture??null);
        this.canvas.removeEventListener('webglcontextlost',this.onLost);this.textures.clear();
    }
}
