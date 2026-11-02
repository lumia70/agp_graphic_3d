export const drawFrame: () => void;
export const initResourceManager: (resMgr: resourceManager.ResourceManager, filesDir: string) => void;
export const setModel: (name: string) => void;
export const setCameraMode: (firstPerson: boolean) => void;
export const setLodBudget: (gaussians: number) => void;
