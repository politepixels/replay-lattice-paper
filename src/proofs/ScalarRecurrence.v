(*
  Copyright 2026 PolitePixels Limited

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*)

Require Import List Arith Lia Floats.
Require Import ReplayLattice ReplayLatticeEval.
Import ListNotations.

(**
  [run_ticks] applies the recurrence step to the accumulator left by the preceding
  updates. The first step receives index zero and the seed.

  Takes [step] ([nat -> A -> A]) which computes the next accumulator from the step
  index and current accumulator.
  Takes [seed] ([A]) which is the initial accumulator.
  Takes [ticks] ([nat]) which is the number of updates to perform.
*)
Fixpoint run_ticks {A : Type} (step : nat -> A -> A) (seed : A) (ticks : nat) : A :=
  match ticks with
  | O => seed
  | S t => step t (run_ticks step seed t)
  end.

(**
  [run_ticks_agree] shows that equal seeds and updates which agree at every step
  give equal final accumulators.

  Takes [left] and [right] ([nat -> A -> A]) which are the two update functions.
  Takes [x] and [y] ([A]) which are their respective seeds.
  Takes [t] ([nat]) which is the number of updates.
*)
Lemma run_ticks_agree {A : Type} (left right : nat -> A -> A) :
  forall x y, x = y -> (forall t a, left t a = right t a) ->
  forall t, run_ticks left x t = run_ticks right y t.
Proof.
  intros x y -> Hstep t.
  induction t as [| t IH].
  - reflexivity.
  - simpl.
    rewrite IH.
    apply Hstep.
Qed.

(**
  [magrees_antitone] transfers agreement between model schedules from a weaker lane
  to a stronger one.

  Takes [a] and [b] ([lane]) which are the stronger and weaker lanes respectively.
  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
*)
Lemma magrees_antitone : forall a b s s',
  a <=l b -> magrees b s s' -> magrees a s s'.
Proof.
  destruct a, b; simpl; try tauto; unfold lane_le; discriminate.
Qed.

(**
  [dag_result_agrees] proves equal binary64 results at a node when schedules agree
  at its assigned lane. The graph must be well-formed and acyclic, with a sound
  lane assignment.

  Takes [P] ([prog]) which is the graph.
  Takes [L] ([nat -> lane]) which assigns lanes to its nodes.
  Takes [i] ([nat]) which identifies the output.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [w] ([nat -> float]) which supplies the common source values.
*)
Lemma dag_result_agrees : forall P L, wf P -> acyclic P -> sound_assignment P L ->
  forall i n s s' w, nth_error P i = Some n -> magrees (L i) s s' ->
    eval64 P i s w = eval64 P i s' w.
Proof.
  intros P L Hwf Hacy HL i.
  induction i as [i IH] using (well_founded_induction lt_wf).
  intros n s s' w Hn Hag.
  rewrite (HL i n Hn) in Hag.

  apply (binary64_assumption1_on_dags P Hwf Hacy i n s s' w Hn).
  - eapply magrees_antitone.
    { apply node_lane_ge_op. }
    exact Hag.
  - intros j Hin.
    pose proof (Hwf i n j Hn Hin) as Hlt.
    destruct (nth_error P j) as [child |] eqn:Hj.
    + apply (IH j Hlt child s s' w Hj).
      eapply magrees_antitone.
      { eapply node_lane_ge_input; exact Hin. }
      exact Hag.
    + apply nth_error_None in Hj.
      assert (i < length P) by (apply nth_error_Some; rewrite Hn; discriminate).
      lia.
Qed.

(**
  [frame] holds either the step index and accumulator of an enclosing loop or the
  scalar value introduced by a binding.
*)
Inductive frame :=
  (**
    [tick_frame] exposes the current recurrence step and the accumulator passed to
    that step.

    Takes [index] ([nat]) which is the zero-based step index.
    Takes [accumulator] ([float]) which is the value carried into the step.
  *)
  | tick_frame (index : nat) (accumulator : float)

  (**
    [binding] adds a scalar value to the enclosing environment.

    Takes [value] ([float]) which is the bound value.
  *)
  | binding (value : float).

(**
  [expr] describes a scalar block, a nested loop or a binding. Each block supplies
  a function that reads its source values from the enclosing frames.
*)
Inductive expr :=
  (**
    [block] embeds an acyclic graph when used with the block classification rule.
    Its source values are read from the surrounding scalar environment.

    Takes [P] ([prog]) which is the embedded graph.
    Takes [out] ([nat]) which identifies its result node.
    Takes [sources] ([list frame -> nat -> float]) which maps the enclosing frames
    and a source index to a value.
  *)
  | block (P : prog) (out : nat) (sources : list frame -> nat -> float)

  (**
    [loop] evaluates the seed once and supplies the previous accumulator to each
    update.

    Takes [ticks] ([nat]) which is the number of updates.
    Takes [seed] and [body] ([expr]) which are the initial expression and the update
    respectively.
  *)
  | loop (ticks : nat) (seed body : expr)

  (**
    [bind_value] evaluates a scalar and makes it available in a fresh frame while
    the body runs.

    Takes [value] and [body] ([expr]) which are the bound expression and the
    expression in its scope.
  *)
  | bind_value (value body : expr).

(**
  [execute] evaluates a scalar expression. Each nested loop starts its own step count
  and its body retains access to the enclosing frames.

  Takes [e] ([expr]) which is the expression to evaluate.
  Takes [s] ([msched]) which is the schedule used by its blocks.
  Takes [env] ([list frame]) which holds the enclosing frames with the innermost
  first.
*)
Fixpoint execute (e : expr) (s : msched) (env : list frame) : float :=
  match e with
  | block P out sources => eval64 P out s (sources env)
  | loop ticks seed body =>
      run_ticks (fun t a => execute body s (tick_frame t a :: env)) (execute seed s env) ticks
  | bind_value value body => execute body s (binding (execute value s env) :: env)
  end.

(**
  [classified] assigns a block its output node's lane under a sound assignment. A
  loop joins the seed and body lanes, and a binding joins the bound value and body
  lanes.
*)
Inductive classified : expr -> lane -> Prop :=
  (**
    [classify_block] gives a block the lane assigned to its output node in a
    well-formed acyclic graph with a sound assignment.

    Takes [P] ([prog]) which is the graph being classified.
    Takes [out] ([nat]) which identifies the result node.
    Takes [sources] ([list frame -> nat -> float]) which reads source values from
    the enclosing frames.
    Takes [L] ([nat -> lane]) which assigns lanes to the graph.
    Takes [n] ([node]) which is the node at [out].
  *)
  | classify_block : forall P out sources L n,
      wf P -> acyclic P -> sound_assignment P L -> nth_error P out = Some n ->
      classified (block P out sources) (L out)

  (**
    [classify_loop] joins the seed lane and the body lane to classify the loop for
    any step count.

    Takes [ticks] ([nat]) which is the number of recurrence steps.
    Takes [seed] and [body] ([expr]) which are the initial expression and the
    repeated update respectively.
    Takes [seed_lane] and [body_lane] ([lane]) which classify those expressions.
  *)
  | classify_loop : forall ticks seed body seed_lane body_lane,
      classified seed seed_lane -> classified body body_lane ->
      classified (loop ticks seed body) (join seed_lane body_lane)

  (**
    [classify_binding] joins the lane of the bound expression with the lane of its
    body.

    Takes [value] and [body] ([expr]) which are the bound expression and the
    expression in its scope.
    Takes [lv] and [body_lane] ([lane]) which classify the value and body
    respectively.
  *)
  | classify_binding : forall value body lv body_lane,
      classified value lv -> classified body body_lane ->
      classified (bind_value value body) (join lv body_lane).

(**
  [result_agrees] proves that a classified scalar expression gives equal results on
  schedules that agree at its lane, including when loops and bindings are nested.

  Takes [e] ([expr]) which is the expression whose value is compared.
  Takes [l] ([lane]) which is its derived lane.
  Takes [s] and [s'] ([msched]) which are the schedules of the two runs.
  Takes [env] ([list frame]) which holds the enclosing frames shared by both runs.
*)
Theorem result_agrees : forall e l, classified e l ->
  forall s s' env, magrees l s s' -> execute e s env = execute e s' env.
Proof.
  intros e l H.
  induction H as
    [ P out sources L n Hwf Hacy HL Hn
    | ticks seed body seed_lane body_lane Hseed IHseed Hbody IHbody
    | value body lv body_lane Hvalue IHvalue Hbody IHbody
    ]; intros s s' env Hag; simpl.
  - eapply dag_result_agrees; eauto.
  - apply run_ticks_agree.
    + apply IHseed.
      eapply magrees_antitone.
      { apply join_ub_l. }
      exact Hag.
    + intros t a.
      apply IHbody.
      eapply magrees_antitone.
      { apply join_ub_r. }
      exact Hag.
  - rewrite (IHvalue s s' env (magrees_antitone _ _ _ _ (join_ub_l lv body_lane) Hag)).
    apply IHbody.
    eapply magrees_antitone.
    { apply join_ub_r. }
    exact Hag.
Qed.

(**
  [carried_agreement] proves that a classified scalar loop has equal accumulators
  after any number of steps on schedules that agree at the join of the seed and
  body lanes.

  Takes [seed] and [body] ([expr]) which are the initial expression and the repeated
  update respectively.
  Takes [seed_lane] and [body_lane] ([lane]) which classify the seed and body
  respectively.
  Takes [s] and [s'] ([msched]) which are the schedules of the two runs.
  Takes [env] ([list frame]) which holds their common enclosing frames.
  Takes [t] ([nat]) which is the number of completed steps.
*)
Corollary carried_agreement : forall seed body seed_lane body_lane,
  classified seed seed_lane -> classified body body_lane ->
  forall s s' env, magrees (join seed_lane body_lane) s s' ->
  forall t, execute (loop t seed body) s env = execute (loop t seed body) s' env.
Proof.
  intros seed body seed_lane body_lane Hseed Hbody s s' env Hag t.
  exact (result_agrees _ _ (classify_loop t seed body seed_lane body_lane Hseed Hbody)
           s s' env Hag).
Qed.

(**
  [read_value] builds a single-source expression whose value is supplied by an
  environment lookup.

  Takes [read] ([list frame -> float]) which computes the source value from the
  enclosing frames.
*)
Definition read_value (read : list frame -> float) :=
  block [lone_source] 0 (fun env _ => read env).

(**
  [constant] builds an expression that returns the same value in every environment.

  Takes [x] ([float]) which is the fixed value.
*)
Definition constant (x : float) := read_value (fun _ => x).

(**
  [current] reads the accumulator from the innermost recurrence frame. It returns
  zero when the leading frame is a binding or the environment is empty.

  Takes [env] ([list frame]) which holds the enclosing frames with the innermost
  first.
*)
Definition current (env : list frame) :=
  match env with tick_frame _ a :: _ => a | _ => 0%float end.

(**
  [nested_constant] checks that the outer loop returns the inner loop's constant
  result of five on every schedule.

  Takes [s] ([msched]) which is the execution schedule.
*)
Example nested_constant : forall s, execute
  (loop 2 (constant 1%float) (loop 3 (constant 0%float) (constant 5%float))) s [] = 5%float.
Proof. reflexivity. Qed.

(**
  [increment] reads the current accumulator and adds one using an exact binary64
  node.
*)
Definition increment := block [lone_source; lone_source; mkNode Exact [0; 1] None false None] 2
  (fun env i => match i with O => current env | _ => 1%float end).

(**
  [nested_increment] checks that each inner loop starts from the current outer
  accumulator. Two outer steps of three increments take the initial one to seven.

  Takes [s] ([msched]) which is the execution schedule.
*)
Example nested_increment : forall s, execute
  (loop 2 (constant 1%float) (loop 3 (read_value current) increment)) s [] = 7%float.
Proof. reflexivity. Qed.
