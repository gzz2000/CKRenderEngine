// Acquire a device before choosing a canvas context: HTML canvases cannot
// switch context types after getContext has succeeded.
export async function createBallanceRenderer(canvas,{backend='auto'}={}) {
    if(!['auto','webgpu','webgl2'].includes(backend))backend='auto';
    let device,reason='WebGPU is unavailable';
    if(backend!=='webgl2'&&navigator.gpu){
        try{const adapter=await navigator.gpu.requestAdapter();if(adapter)device=await adapter.requestDevice();else reason='WebGPU adapter is unavailable';}
        catch(error){reason=String(error.message||error);}
    }
    const load=async name=>{const response=await fetch(new URL(name,import.meta.url));if(!response.ok)throw new Error(`${name}: HTTP ${response.status}`);return response.json();};
    if(device){
        const {createBallanceGpu}=await import('./webgpu.mjs');
        try{return await createBallanceGpu(canvas,await load('shaders.json'),device);}catch(error){device.destroy();throw error;}
    }
    if(backend==='webgpu')throw new Error(reason);
    const {createBallanceGl}=await import('./webgl2.mjs');
    const renderer=await createBallanceGl(canvas,await load('shaders-gl.json'));
    renderer.fallbackReason=backend==='auto'?reason:null;
    return renderer;
}
