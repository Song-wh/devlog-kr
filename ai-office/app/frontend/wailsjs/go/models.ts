export namespace claudecode {
	
	export class AuthStatus {
	    loggedIn: boolean;
	    authMethod: string;
	    email?: string;
	    orgName?: string;
	    plan?: string;
	    error?: string;
	
	    static createFrom(source: any = {}) {
	        return new AuthStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.loggedIn = source["loggedIn"];
	        this.authMethod = source["authMethod"];
	        this.email = source["email"];
	        this.orgName = source["orgName"];
	        this.plan = source["plan"];
	        this.error = source["error"];
	    }
	}

}

export namespace main {
	
	export class Detail {
	    request: store.RequestRow;
	    tasks: store.TaskRow[];
	    checks: store.CheckRow[];
	    report: string;
	    diff: string;
	
	    static createFrom(source: any = {}) {
	        return new Detail(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.request = this.convertValues(source["request"], store.RequestRow);
	        this.tasks = this.convertValues(source["tasks"], store.TaskRow);
	        this.checks = this.convertValues(source["checks"], store.CheckRow);
	        this.report = source["report"];
	        this.diff = source["diff"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class EmployeeView {
	    id: number;
	    name: string;
	    role: string;
	    model: string;
	    prompt: string;
	    connection: string;
	    authMode: string;
	    createdAt: string;
	    status: claudecode.AuthStatus;
	    loggingIn: boolean;
	
	    static createFrom(source: any = {}) {
	        return new EmployeeView(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.name = source["name"];
	        this.role = source["role"];
	        this.model = source["model"];
	        this.prompt = source["prompt"];
	        this.connection = source["connection"];
	        this.authMode = source["authMode"];
	        this.createdAt = source["createdAt"];
	        this.status = this.convertValues(source["status"], claudecode.AuthStatus);
	        this.loggingIn = source["loggingIn"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class Project {
	    repo: string;
	    checks: string[];
	    protect: string[];
	
	    static createFrom(source: any = {}) {
	        return new Project(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.repo = source["repo"];
	        this.checks = source["checks"];
	        this.protect = source["protect"];
	    }
	}
	export class StartInput {
	    project: Project;
	    request: string;
	    mode: string;
	    team: Record<string, number>;
	    budgetUsd: number;
	    fixRounds: number;
	
	    static createFrom(source: any = {}) {
	        return new StartInput(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.project = this.convertValues(source["project"], Project);
	        this.request = source["request"];
	        this.mode = source["mode"];
	        this.team = source["team"];
	        this.budgetUsd = source["budgetUsd"];
	        this.fixRounds = source["fixRounds"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}

}

export namespace store {
	
	export class CheckRow {
	    round: number;
	    command: string;
	    exitCode: number;
	    timedOut: boolean;
	    seconds: number;
	    protected: string;
	    logPath: string;
	
	    static createFrom(source: any = {}) {
	        return new CheckRow(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.round = source["round"];
	        this.command = source["command"];
	        this.exitCode = source["exitCode"];
	        this.timedOut = source["timedOut"];
	        this.seconds = source["seconds"];
	        this.protected = source["protected"];
	        this.logPath = source["logPath"];
	    }
	}
	export class Employee {
	    id: number;
	    name: string;
	    role: string;
	    model: string;
	    prompt: string;
	    connection: string;
	    authMode: string;
	    createdAt: string;
	
	    static createFrom(source: any = {}) {
	        return new Employee(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.name = source["name"];
	        this.role = source["role"];
	        this.model = source["model"];
	        this.prompt = source["prompt"];
	        this.connection = source["connection"];
	        this.authMode = source["authMode"];
	        this.createdAt = source["createdAt"];
	    }
	}
	export class RequestRow {
	    id: string;
	    text: string;
	    repo: string;
	    worktree: string;
	    branch: string;
	    base: string;
	    status: string;
	    reason: string;
	    plan: string;
	    config: string;
	    createdAt: string;
	    finishedAt: string;
	    costUsd: number;
	    runs: number;
	
	    static createFrom(source: any = {}) {
	        return new RequestRow(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.text = source["text"];
	        this.repo = source["repo"];
	        this.worktree = source["worktree"];
	        this.branch = source["branch"];
	        this.base = source["base"];
	        this.status = source["status"];
	        this.reason = source["reason"];
	        this.plan = source["plan"];
	        this.config = source["config"];
	        this.createdAt = source["createdAt"];
	        this.finishedAt = source["finishedAt"];
	        this.costUsd = source["costUsd"];
	        this.runs = source["runs"];
	    }
	}
	export class TaskRow {
	    id: number;
	    seq: number;
	    kind: string;
	    role: string;
	    title: string;
	    status: string;
	    result: string;
	    costUsd: number;
	    seconds: number;
	    estimated: boolean;
	
	    static createFrom(source: any = {}) {
	        return new TaskRow(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.seq = source["seq"];
	        this.kind = source["kind"];
	        this.role = source["role"];
	        this.title = source["title"];
	        this.status = source["status"];
	        this.result = source["result"];
	        this.costUsd = source["costUsd"];
	        this.seconds = source["seconds"];
	        this.estimated = source["estimated"];
	    }
	}

}

