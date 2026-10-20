// '@angular/core' for Celio-Client's WebSocketService: @Injectable is the only
// thing it uses, and outside Angular's injector it has nothing to do.
export function Injectable() { return (target) => target; }
